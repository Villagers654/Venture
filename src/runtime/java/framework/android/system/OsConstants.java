package android.system;

/** The Linux errno and file constants of AOSP android.system.OsConstants that apps compare against. */
public final class OsConstants {
  private OsConstants() {}
  public static final int EPERM = 1, ENOENT = 2, EINTR = 4, EIO = 5, EBADF = 9, EAGAIN = 11, ENOMEM = 12,
      EACCES = 13, EEXIST = 17, ENOTDIR = 20, EISDIR = 21, EINVAL = 22, ENOSPC = 28, ESPIPE = 29, EROFS = 30,
      EPIPE = 32, ERANGE = 34, ENOSYS = 38, ETIMEDOUT = 110, ECONNREFUSED = 111;
  public static final int O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2, O_CREAT = 64, O_EXCL = 128, O_TRUNC = 512,
      O_APPEND = 1024, O_NONBLOCK = 2048, O_CLOEXEC = 524288;
  public static final int SEEK_SET = 0, SEEK_CUR = 1, SEEK_END = 2;
  public static final int F_OK = 0, X_OK = 1, W_OK = 2, R_OK = 4;
  public static final int S_IFMT = 61440, S_IFREG = 32768, S_IFDIR = 16384;
  public static boolean S_ISREG(int mode) { return (mode & S_IFMT) == S_IFREG; }
  public static boolean S_ISDIR(int mode) { return (mode & S_IFMT) == S_IFDIR; }
  public static String errnoName(int errno) {
    return switch (errno) {
      case EPERM -> "EPERM"; case ENOENT -> "ENOENT"; case EINTR -> "EINTR"; case EIO -> "EIO"; case EBADF -> "EBADF";
      case EAGAIN -> "EAGAIN"; case ENOMEM -> "ENOMEM"; case EACCES -> "EACCES"; case EEXIST -> "EEXIST";
      case ENOTDIR -> "ENOTDIR"; case EISDIR -> "EISDIR"; case EINVAL -> "EINVAL"; case ENOSPC -> "ENOSPC";
      case ESPIPE -> "ESPIPE"; case EROFS -> "EROFS"; case EPIPE -> "EPIPE"; case ERANGE -> "ERANGE";
      case ENOSYS -> "ENOSYS"; case ETIMEDOUT -> "ETIMEDOUT"; case ECONNREFUSED -> "ECONNREFUSED";
      default -> null;
    };
  }
}
