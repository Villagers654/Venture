.class public LBusyData;
.super Ljava/lang/Object;

.field public value:I
.field public marked:Z

.method public constructor <init>()V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    const/4 v0, 0x1
    iput-boolean v0, p0, LBusyData;->marked:Z
    return-void
.end method
