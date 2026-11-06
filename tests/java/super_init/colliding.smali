.class public LCollidingError;
.super Ljava/lang/RuntimeException;

.field public marked:Z

.method public constructor <init>(Ljava/lang/String;)V
    .registers 3
    invoke-direct {p0, p1}, Ljava/lang/RuntimeException;-><init>(Ljava/lang/String;)V
    const/4 v0, 0x1
    iput-boolean v0, p0, LCollidingError;->marked:Z
    return-void
.end method
