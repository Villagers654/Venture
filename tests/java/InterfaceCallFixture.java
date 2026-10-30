import java.nio.file.*;
import org.objectweb.asm.*;

/**
 * Writes the bytecode dex2jar emits for DEX invoke-static on an interface method: a Methodref,
 * not an InterfaceMethodref. ItfCaller.call() fails with IncompatibleClassChangeError unless the
 * rewriter marks the reference as an interface method.
 */
public class InterfaceCallFixture {
  public static void main(String[] args) throws Exception {
    Path out = Path.of(args[0]);
    var itf = new ClassWriter(ClassWriter.COMPUTE_FRAMES | ClassWriter.COMPUTE_MAXS);
    itf.visit(Opcodes.V1_8, Opcodes.ACC_PUBLIC | Opcodes.ACC_INTERFACE | Opcodes.ACC_ABSTRACT, "ItfStatic", null, "java/lang/Object", null);
    var body = itf.visitMethod(Opcodes.ACC_PUBLIC | Opcodes.ACC_STATIC, "value", "()Ljava/lang/String;", null, null);
    body.visitCode(); body.visitLdcInsn("interface static"); body.visitInsn(Opcodes.ARETURN); body.visitMaxs(0, 0); body.visitEnd();
    itf.visitEnd();
    Files.write(out.resolve("ItfStatic.class"), itf.toByteArray());
    var caller = new ClassWriter(ClassWriter.COMPUTE_FRAMES | ClassWriter.COMPUTE_MAXS);
    caller.visit(Opcodes.V1_8, Opcodes.ACC_PUBLIC, "ItfCaller", null, "java/lang/Object", null);
    var call = caller.visitMethod(Opcodes.ACC_PUBLIC | Opcodes.ACC_STATIC, "call", "()Ljava/lang/String;", null, null);
    call.visitCode();
    call.visitMethodInsn(Opcodes.INVOKESTATIC, "ItfStatic", "value", "()Ljava/lang/String;", false);
    call.visitInsn(Opcodes.ARETURN); call.visitMaxs(0, 0); call.visitEnd();
    caller.visitEnd();
    Files.write(out.resolve("ItfCaller.class"), caller.toByteArray());
  }
}
