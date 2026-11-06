# R8 output that ART accepts and the JVM does not: a new-instance of C initialized by a
# constructor of one of C's superclasses (R8 inlines C's constructor when it only forwards
# to super). Open Brush's androidx InitializationProvider.onCreate (new-instance of an
# exception class, RuntimeException.<init>(Throwable)) and Unity's UnityPlayer.<init>
# (new-instance of a data class, Object.<init>()) are this pattern. Each method returns
# the constructed object; SuperInitCheck asserts its class and state under -Xverify:all.
.class public LSuperInitCallers;
.super Ljava/lang/Object;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

# Direct superclass with arguments; the target class is in the other DEX.
.method public static wrap(Ljava/lang/Throwable;)Ljava/lang/Object;
    .registers 2
    new-instance v0, LMergedError;
    invoke-direct {v0, p0}, Ljava/lang/RuntimeException;-><init>(Ljava/lang/Throwable;)V
    return-object v0
.end method

# Object.<init> for a plain class, then fields are stored (UnityPlayer's k0/h).
.method public static plain(I)Ljava/lang/Object;
    .registers 2
    new-instance v0, LPlainData;
    invoke-direct {v0}, Ljava/lang/Object;-><init>()V
    iput p0, v0, LPlainData;->value:I
    return-object v0
.end method

# A grandparent constructor: MergedChild extends MergedMiddle extends Exception, and
# neither app class declares (String).
.method public static grand(Ljava/lang/String;)Ljava/lang/Object;
    .registers 2
    new-instance v0, LMergedChild;
    invoke-direct {v0, p0}, Ljava/lang/Exception;-><init>(Ljava/lang/String;)V
    return-object v0
.end method

# The target already declares a different constructor with the superclass's descriptor:
# it must not run (its marker field stays false).
.method public static collide(Ljava/lang/String;)Ljava/lang/Object;
    .registers 2
    new-instance v0, LCollidingError;
    invoke-direct {v0, p0}, Ljava/lang/RuntimeException;-><init>(Ljava/lang/String;)V
    return-object v0
.end method

# The uninitialized reference is copied and crosses a branch before initialization,
# inside a catch handler as in InitializationProvider.onCreate.
.method public static handler(Ljava/lang/String;Z)Ljava/lang/Object;
    .registers 5
    :try_start
    invoke-static {p0}, Ljava/lang/Integer;->parseInt(Ljava/lang/String;)I
    :try_end
    .catch Ljava/lang/NumberFormatException; {:try_start .. :try_end} :caught
    const/4 v0, 0x0
    return-object v0
    :caught
    move-exception v0
    new-instance v1, LMergedError;
    move-object v2, v1
    if-eqz p1, :plain
    const-string v3, "flagged"
    goto :join
    :plain
    const-string v3, "plain"
    :join
    invoke-direct {v2, v3, v0}, Ljava/lang/RuntimeException;-><init>(Ljava/lang/String;Ljava/lang/Throwable;)V
    return-object v1
.end method

# Ordinary construction of the same classes stays as written.
.method public static ordinary()Ljava/lang/Object;
    .registers 2
    new-instance v0, LCollidingError;
    const-string v1, "ordinary"
    invoke-direct {v0, v1}, LCollidingError;-><init>(Ljava/lang/String;)V
    return-object v0
.end method

# Object.<init>() on a class whose own ()V constructor does more (androidx FragmentManager's y):
# no argument to tell them apart, so the call passes a null marker from a new register; the
# method's parameters still work after the register shift.
.method public static noargs(I)Ljava/lang/Object;
    .registers 2
    new-instance v0, LBusyData;
    invoke-direct {v0}, Ljava/lang/Object;-><init>()V
    iput p0, v0, LBusyData;->value:I
    return-object v0
.end method
