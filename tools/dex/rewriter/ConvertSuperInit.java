package com.venture.dex;

import java.nio.file.*;
import java.util.*;
import com.googlecode.d2j.DexConstants;
import com.googlecode.d2j.Method;
import com.googlecode.d2j.node.*;
import com.googlecode.d2j.node.analysis.*;
import com.googlecode.d2j.node.insn.*;
import com.googlecode.d2j.reader.DexFileReader;
import com.googlecode.d2j.reader.Op;
import com.googlecode.d2j.visitors.DexCodeVisitor;
import com.googlecode.d2j.visitors.DexDebugVisitor;

/** ART lets invoke-direct initialize a new-instance of C with a constructor of any superclass S
 * of C; R8 relies on that when it inlines a constructor of C that only forwards to super(...),
 * and then often removes it. The JVM (JVMS 4.10.1.9 invokespecial) requires C's own <init>.
 * dex2jar 2.4 does not reject the pattern: it emits "new S" (an object of the wrong class, so
 * later stores/returns fail verification: Unity's UnityPlayer.<init>) or keeps "new C" with
 * S.<init> ("Call to wrong <init> method": Open Brush's androidx InitializationProvider).
 *
 * Before translation, each such call is retargeted to a public synthetic constructor of C with
 * S's parameters that only calls S.<init>, which is exactly what ART executed (no constructor
 * body of C ran). Classes between C and S get the same forwarding constructor. When C already
 * declares a different constructor with that descriptor (R8 kept it), the synthetic one also
 * takes null Void markers. The same applies to "this" in a constructor initialized by a
 * superclass above its direct parent. Receivers come from a register dataflow
 * over the method (copies, branches, catch handlers); an unknown or merged receiver is left as is.
 */
final class ConvertSuperInit {
    private static final Object THIS = new Object() { public String toString() { return "this"; } };
    private static final Object UNKNOWN = new Object() { public String toString() { return "?"; } };
    private static final String MARKER = "Ljava/lang/Void;";
    private static final int FLAGS = DexFileReader.KEEP_ALL_METHODS | DexFileReader.SKIP_DEBUG;

    /** A call-site retarget: the n-th invoke-direct <init> of a method calls owner.<init>(params). */
    record Site(String owner, String[] params, int markers) {}
    /** A synthetic constructor: owner.<init>(params) forwards to next.<init>(nextParams). */
    record Synthetic(String owner, String[] params, String next, String[] nextParams, int nextMarkers) {}

    /** The ordinal-th invoke-direct <init> of method initializes a target with superclass.<init>(params). */
    record Request(String method, int ordinal, String target, String superclass, String[] params) {}
    private final List<Request> requests = new ArrayList<>();
    final Map<String, String> parents = new HashMap<>();
    final Map<String, Set<String>> constructors = new HashMap<>();
    final Map<String, Map<Integer, Site>> sites = new HashMap<>();  // "Lowner;->name(desc)" -> ordinal -> site
    final Map<String, List<Synthetic>> synthetics = new HashMap<>();
    private final Map<String, String[]> planned = new HashMap<>();   // owner|S|desc -> chosen params
    private final Set<String> claimed = new HashSet<>();             // owner|desc of synthetic constructors
    final List<String> unresolved = new ArrayList<>();
    int retargeted;

    static ConvertSuperInit plan(List<Path> dexes) throws Exception {
        ConvertSuperInit plan = new ConvertSuperInit();
        // A call site's target class can be in any DEX of the APK: find the calls per DEX,
        // then plan constructors once every class header is known.
        for (Path path : dexes) {
            DexFileNode dex = new DexFileNode();
            new DexFileReader(path).accept(dex, FLAGS);
            for (DexClassNode cls : dex.clzs) {
                plan.parents.put(cls.className, cls.superClass);
                Set<String> declared = new HashSet<>();
                if (cls.methods != null)
                    for (DexMethodNode m : cls.methods)
                        if (m.method.getName().equals("<init>")) declared.add(m.method.getDesc());
                plan.constructors.put(cls.className, declared);
            }
            for (DexClassNode cls : dex.clzs)
                if (cls.methods != null)
                    for (DexMethodNode m : cls.methods)
                        if (candidate(m)) plan.analyze(cls, m);
        }
        for (Request r : plan.requests) {
            Site site = plan.retarget(r.target(), r.superclass(), r.params());
            if (site == null) plan.unresolved.add(r.method() + "\t" + r.target() + " via " + r.superclass() + ".<init>" + desc(r.params()));
            else { plan.sites.computeIfAbsent(r.method(), k -> new HashMap<>()).put(r.ordinal(), site); ++plan.retargeted; }
        }
        plan.requests.clear();
        return plan;
    }

    /** Cheap filter: a new-instance type with fewer <init> calls on it than allocations, or a
     * constructor initializing "this" through a class that is neither it nor its parent. */
    private static boolean candidate(DexMethodNode m) {
        if (m.codeNode == null || m.codeNode.stmts == null) return false;
        Map<String, Integer> balance = new HashMap<>();
        boolean foreign = false;
        for (DexStmtNode s : m.codeNode.stmts) {
            if (s instanceof TypeStmtNode t && t.op == Op.NEW_INSTANCE) balance.merge(t.type, 1, Integer::sum);
            else if (init(s) instanceof MethodStmtNode call) {
                balance.merge(call.method.getOwner(), -1, Integer::sum);
                foreign = true;
            }
        }
        if (balance.values().stream().anyMatch(v -> v > 0)) return true;
        // Constructors: an <init> on a type never allocated here may be the "this" call.
        return foreign && m.method.getName().equals("<init>") && (m.access & DexConstants.ACC_STATIC) == 0
            && balance.values().stream().anyMatch(v -> v < 0);
    }

    private static MethodStmtNode init(DexStmtNode s) {
        return s instanceof MethodStmtNode call && (s.op == Op.INVOKE_DIRECT || s.op == Op.INVOKE_DIRECT_RANGE)
            && call.method.getName().equals("<init>") && call.args.length > 0 ? call : null;
    }

    static String key(Method m) { return m.getOwner() + "->" + m.getName() + m.getDesc(); }

    private void analyze(DexClassNode cls, DexMethodNode m) {
        DexCodeNode code = m.codeNode;
        List<DexStmtNode> stmts = code.stmts;
        int n = stmts.size();
        Map<com.googlecode.d2j.DexLabel, Integer> labels = new HashMap<>();
        for (int i = 0; i < n; ++i) if (stmts.get(i) instanceof DexLabelStmtNode l) labels.put(l.label, i);
        @SuppressWarnings("unchecked") DvmFrame<Object>[] frames = new DvmFrame[n];
        Deque<Integer> work = new ArrayDeque<>();
        int registers = code.totalRegister;
        DvmFrame<Object> entry = blank(registers);
        if (m.method.getName().equals("<init>") && (m.access & DexConstants.ACC_STATIC) == 0) {
            int ins = 1;
            for (String p : m.method.getParameterTypes()) ins += p.equals("J") || p.equals("D") ? 2 : 1;
            if (registers - ins >= 0) entry.setReg(registers - ins, THIS);
        }
        if (n == 0) return;
        frames[0] = entry; work.add(0);
        // A catch handler starts from unknown registers: R8 does not carry an uninitialized
        // reference into a handler, and an unknown receiver only means "leave the call as is".
        if (code.tryStmts != null)
            for (TryCatchNode t : code.tryStmts)
                for (var h : t.handler) {
                    Integer at = labels.get(h);
                    if (at != null && frames[at] == null) { frames[at] = blank(registers); work.add(at); }
                }
        Interpreter interpreter = new Interpreter();
        try {
            while (!work.isEmpty()) {
                int i = work.poll();
                DexStmtNode s = stmts.get(i);
                DvmFrame<Object> after = blank(registers).init(frames[i]);
                after.setTmp(frames[i].getTmp());
                if (s.op != null && s.op != Op.NOP) after.execute(s, interpreter);  // DvmFrame rejects nop
                List<Integer> next = new ArrayList<>();
                if (s.op == null || s.op.canContinue()) next.add(i + 1);
                if (s instanceof JumpStmtNode j) next.add(labels.get(j.label));
                if (s instanceof BaseSwitchStmtNode sw) for (var l : sw.labels) next.add(labels.get(l));
                for (Integer t : next) {
                    if (t == null || t >= n) continue;
                    if (frames[t] == null) { frames[t] = blank(registers).init(after); frames[t].setTmp(after.getTmp()); work.add(t); }
                    else if (merge(frames[t], after)) work.add(t);
                }
            }
        } catch (RuntimeException e) {
            unresolved.add(key(m.method) + "\tanalysis " + e);
            return;
        }
        int ordinal = -1;
        for (int i = 0; i < n; ++i) {
            MethodStmtNode call = init(stmts.get(i));
            if (call == null) continue;
            ++ordinal;
            if (frames[i] == null) continue;  // unreachable
            Object receiver = frames[i].getReg(call.args[0]);
            String owner = call.method.getOwner(), target;
            if (receiver instanceof TypeStmtNode allocation) {
                target = allocation.type;
                if (target.equals(owner)) continue;
            } else if (receiver == THIS) {
                String parent = parents.get(cls.className);
                if (owner.equals(cls.className) || owner.equals(parent)) continue;
                target = parent;
            } else continue;
            requests.add(new Request(key(m.method), ordinal, target, owner, call.method.getParameterTypes()));
        }
    }

    /** Plans forwarding constructors from target up to superclass; null if a class between them
     * is not in the APK (its constructors cannot be added) or no free descriptor exists. */
    private Site retarget(String target, String superclass, String[] params) {
        List<String> chain = new ArrayList<>();
        for (String c = target; !c.equals(superclass); c = parents.get(c)) {
            if (c == null || !parents.containsKey(c) || chain.contains(c)) return null;
            chain.add(c);
        }
        String next = superclass;
        String[] nextParams = params;
        for (int i = chain.size() - 1; i >= 0; --i) {
            String owner = chain.get(i);
            String plannedKey = owner + "|" + next + "|" + String.join("", params);
            String[] chosen = planned.get(plannedKey);
            if (chosen == null) {
                chosen = free(owner, params);
                if (chosen == null) return null;
                planned.put(plannedKey, chosen);
                claimed.add(owner + "|" + desc(chosen));
                synthetics.computeIfAbsent(owner, k -> new ArrayList<>())
                    .add(new Synthetic(owner, chosen, next, nextParams, nextParams.length - params.length));
            }
            next = owner; nextParams = chosen;
        }
        return new Site(next, nextParams, nextParams.length - params.length);
    }

    /** The superclass's parameters, then as many null Void markers as make the descriptor new to owner
     * (owner may declare a different constructor with the superclass's descriptor, R8 kept it). */
    private String[] free(String owner, String[] params) {
        Set<String> declared = constructors.getOrDefault(owner, Set.of());
        for (int extra = 0; extra <= 8; ++extra) {
            String[] p = Arrays.copyOf(params, params.length + extra);
            Arrays.fill(p, params.length, p.length, MARKER);
            String d = desc(p);
            if (!declared.contains(d) && !claimed.contains(owner + "|" + d)) return p;
        }
        return null;
    }

    static String desc(String[] params) { return "(" + String.join("", params) + ")V"; }

    /** Applies the plan to one DEX before translation. */
    void apply(DexFileNode dex) {
        for (DexClassNode cls : dex.clzs) {
            if (cls.methods != null)
                for (DexMethodNode m : cls.methods) {
                    Map<Integer, Site> at = sites.get(key(m.method));
                    if (at != null && m.codeNode != null) m.codeNode = retargeted(m.codeNode, at);
                }
            List<Synthetic> added = synthetics.get(cls.className);
            if (added == null) continue;
            if (cls.methods == null) cls.methods = new ArrayList<>();
            for (Synthetic s : added) cls.methods.add(forwarder(s));
        }
    }

    /** The method's code with its planned calls retargeted. A marker argument needs a register
     * holding null: every register moves up by one (parameters stay the last registers) and v0
     * is set to null right before each such call. */
    private static DexCodeNode retargeted(DexCodeNode code, Map<Integer, Site> at) {
        int shift = at.values().stream().anyMatch(s -> s.markers() > 0) ? 1 : 0;
        DexCodeNode out = new DexCodeNode();
        code.accept(new Shift(out, shift) {
            int ordinal = -1;
            @Override public void visitMethodStmt(Op op, int[] args, Method method) {
                if ((op == Op.INVOKE_DIRECT || op == Op.INVOKE_DIRECT_RANGE) && method.getName().equals("<init>") && args.length > 0) {
                    Site site = at.get(++ordinal);
                    if (site != null) {
                        int[] moved = Arrays.copyOf(args, args.length + site.markers());
                        for (int i = 0; i < args.length; ++i) moved[i] = args[i] + shift;
                        Arrays.fill(moved, args.length, moved.length, 0);
                        if (site.markers() > 0) out.visitConstStmt(Op.CONST_4, 0, 0);
                        out.visitMethodStmt(op, moved, new Method(site.owner(), "<init>", site.params(), "V"));
                        return;
                    }
                }
                super.visitMethodStmt(op, args, method);
            }
        });
        return out;
    }

    /** Forwards this and the shared parameters to next's constructor, with v0 = null as its markers. */
    private static DexMethodNode forwarder(Synthetic s) {
        DexMethodNode m = new DexMethodNode(DexConstants.ACC_PUBLIC | DexConstants.ACC_SYNTHETIC | DexConstants.ACC_CONSTRUCTOR,
            new Method(s.owner(), "<init>", s.params(), "V"));
        int ins = 1;
        for (String p : s.params()) ins += p.equals("J") || p.equals("D") ? 2 : 1;
        int forwarded = 1;  // this, then the parameters before any marker
        for (int k = 0; k < s.nextParams().length - s.nextMarkers(); ++k) {
            String p = s.nextParams()[k];
            forwarded += p.equals("J") || p.equals("D") ? 2 : 1;
        }
        // v0 is a local; "this" and the parameters are the last registers, v1 onwards.
        int[] call = new int[forwarded + s.nextMarkers()];
        for (int r = 0; r < forwarded; ++r) call[r] = 1 + r;
        DexCodeNode code = new DexCodeNode();
        code.visitRegister(1 + ins);
        if (s.nextMarkers() > 0) code.visitConstStmt(Op.CONST_4, 0, 0);
        code.visitMethodStmt(Op.INVOKE_DIRECT, call, new Method(s.next(), "<init>", s.nextParams(), "V"));
        code.visitStmt0R(Op.RETURN_VOID);
        m.codeNode = code;
        return m;
    }

    /** Replays code with every register number increased by shift (debug locals included). */
    private static class Shift extends DexCodeVisitor {
        final int shift;
        Shift(DexCodeVisitor out, int shift) { super(out); this.shift = shift; }
        int r(int register) { return register < 0 ? register : register + shift; }
        int[] r(int[] registers) {
            int[] out = registers.clone();
            for (int i = 0; i < out.length; ++i) out[i] = r(out[i]);
            return out;
        }
        @Override public void visitRegister(int total) { super.visitRegister(total + shift); }
        @Override public void visitStmt1R(Op op, int a) { super.visitStmt1R(op, r(a)); }
        @Override public void visitStmt2R(Op op, int a, int b) { super.visitStmt2R(op, r(a), r(b)); }
        @Override public void visitStmt2R1N(Op op, int a, int b, int n) { super.visitStmt2R1N(op, r(a), r(b), n); }
        @Override public void visitStmt3R(Op op, int a, int b, int c) { super.visitStmt3R(op, r(a), r(b), r(c)); }
        @Override public void visitTypeStmt(Op op, int a, int b, String type) { super.visitTypeStmt(op, r(a), r(b), type); }
        @Override public void visitConstStmt(Op op, int a, Object value) { super.visitConstStmt(op, r(a), value); }
        @Override public void visitFillArrayDataStmt(Op op, int a, Object array) { super.visitFillArrayDataStmt(op, r(a), array); }
        @Override public void visitFieldStmt(Op op, int a, int b, com.googlecode.d2j.Field f) { super.visitFieldStmt(op, r(a), r(b), f); }
        @Override public void visitFilledNewArrayStmt(Op op, int[] args, String type) { super.visitFilledNewArrayStmt(op, r(args), type); }
        @Override public void visitJumpStmt(Op op, int a, int b, com.googlecode.d2j.DexLabel l) { super.visitJumpStmt(op, r(a), r(b), l); }
        @Override public void visitMethodStmt(Op op, int[] args, Method m) { super.visitMethodStmt(op, r(args), m); }
        @Override public void visitMethodStmt(Op op, int[] args, com.googlecode.d2j.CallSite c) { super.visitMethodStmt(op, r(args), c); }
        @Override public void visitMethodStmt(Op op, int[] args, Method m, com.googlecode.d2j.Proto p) { super.visitMethodStmt(op, r(args), m, p); }
        @Override public void visitPackedSwitchStmt(Op op, int a, int first, com.googlecode.d2j.DexLabel[] l) { super.visitPackedSwitchStmt(op, r(a), first, l); }
        @Override public void visitSparseSwitchStmt(Op op, int a, int[] keys, com.googlecode.d2j.DexLabel[] l) { super.visitSparseSwitchStmt(op, r(a), keys, l); }
        @Override public DexDebugVisitor visitDebug() {
            DexDebugVisitor inner = super.visitDebug();
            if (inner == null || shift == 0) return inner;
            return new DexDebugVisitor(inner) {
                @Override public void visitStartLocal(int reg, com.googlecode.d2j.DexLabel l, String name, String type, String sig) { super.visitStartLocal(r(reg), l, name, type, sig); }
                @Override public void visitEndLocal(int reg, com.googlecode.d2j.DexLabel l) { super.visitEndLocal(r(reg), l); }
                @Override public void visitRestartLocal(int reg, com.googlecode.d2j.DexLabel l) { super.visitRestartLocal(r(reg), l); }
            };
        }
    }

    private static DvmFrame<Object> blank(int registers) {
        DvmFrame<Object> f = new DvmFrame<>(registers);
        for (int r = 0; r < registers; ++r) f.setReg(r, UNKNOWN);
        f.setTmp(UNKNOWN);
        return f;
    }

    private static boolean merge(DvmFrame<Object> into, DvmFrame<Object> from) {
        boolean changed = false;
        for (int r = 0; r < into.getTotalRegisters(); ++r) {
            Object a = into.getReg(r), b = from.getReg(r);
            if (a != UNKNOWN && a != b) { into.setReg(r, UNKNOWN); changed = true; }
        }
        if (into.getTmp() != UNKNOWN && into.getTmp() != from.getTmp()) { into.setTmp(UNKNOWN); changed = true; }
        return changed;
    }

    /** Tracks only where a reference came from: its new-instance, "this", or unknown. */
    private static final class Interpreter extends DvmInterpreter<Object> {
        @Override public Object newOperation(DexStmtNode s) { return s.op == Op.NEW_INSTANCE ? s : UNKNOWN; }
        @Override public Object copyOperation(DexStmtNode s, Object v) { return v == null ? UNKNOWN : v; }
        @Override public Object unaryOperation(DexStmtNode s, Object v) { return UNKNOWN; }
        @Override public Object binaryOperation(DexStmtNode s, Object a, Object b) { return UNKNOWN; }
        @Override public Object ternaryOperation(DexStmtNode s, Object a, Object b, Object c) { return UNKNOWN; }
        @Override public Object naryOperation(DexStmtNode s, List<?> v) { return UNKNOWN; }
        @Override public void returnOperation(DexStmtNode s, Object v) {}
    }
}
