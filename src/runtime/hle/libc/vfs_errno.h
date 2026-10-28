#pragma once
// Preserve host errno macros while taking constants from the Linux contract.
#pragma push_macro("EPERM")
#undef EPERM
#pragma push_macro("ENOENT")
#undef ENOENT
#pragma push_macro("ESRCH")
#undef ESRCH
#pragma push_macro("EINTR")
#undef EINTR
#pragma push_macro("EIO")
#undef EIO
#pragma push_macro("ENXIO")
#undef ENXIO
#pragma push_macro("E2BIG")
#undef E2BIG
#pragma push_macro("ENOEXEC")
#undef ENOEXEC
#pragma push_macro("EBADF")
#undef EBADF
#pragma push_macro("ECHILD")
#undef ECHILD
#pragma push_macro("EAGAIN")
#undef EAGAIN
#pragma push_macro("ENOMEM")
#undef ENOMEM
#pragma push_macro("EACCES")
#undef EACCES
#pragma push_macro("EFAULT")
#undef EFAULT
#pragma push_macro("ENOTBLK")
#undef ENOTBLK
#pragma push_macro("EBUSY")
#undef EBUSY
#pragma push_macro("EEXIST")
#undef EEXIST
#pragma push_macro("EXDEV")
#undef EXDEV
#pragma push_macro("ENODEV")
#undef ENODEV
#pragma push_macro("ENOTDIR")
#undef ENOTDIR
#pragma push_macro("EISDIR")
#undef EISDIR
#pragma push_macro("EINVAL")
#undef EINVAL
#pragma push_macro("ENFILE")
#undef ENFILE
#pragma push_macro("EMFILE")
#undef EMFILE
#pragma push_macro("ENOTTY")
#undef ENOTTY
#pragma push_macro("ETXTBSY")
#undef ETXTBSY
#pragma push_macro("EFBIG")
#undef EFBIG
#pragma push_macro("ENOSPC")
#undef ENOSPC
#pragma push_macro("ESPIPE")
#undef ESPIPE
#pragma push_macro("EROFS")
#undef EROFS
#pragma push_macro("EMLINK")
#undef EMLINK
#pragma push_macro("EPIPE")
#undef EPIPE
#pragma push_macro("EDOM")
#undef EDOM
#pragma push_macro("ERANGE")
#undef ERANGE
#pragma push_macro("EDEADLK")
#undef EDEADLK
#pragma push_macro("ENAMETOOLONG")
#undef ENAMETOOLONG
#pragma push_macro("ENOLCK")
#undef ENOLCK
#pragma push_macro("ENOSYS")
#undef ENOSYS
#pragma push_macro("ENOTEMPTY")
#undef ENOTEMPTY
#pragma push_macro("ELOOP")
#undef ELOOP
#pragma push_macro("ENOMSG")
#undef ENOMSG
#pragma push_macro("EIDRM")
#undef EIDRM
#pragma push_macro("ENOSTR")
#undef ENOSTR
#pragma push_macro("ENODATA")
#undef ENODATA
#pragma push_macro("ETIME")
#undef ETIME
#pragma push_macro("ENOSR")
#undef ENOSR
#pragma push_macro("EREMOTE")
#undef EREMOTE
#pragma push_macro("ENOLINK")
#undef ENOLINK
#pragma push_macro("EPROTO")
#undef EPROTO
#pragma push_macro("EMULTIHOP")
#undef EMULTIHOP
#pragma push_macro("EBADMSG")
#undef EBADMSG
#pragma push_macro("EOVERFLOW")
#undef EOVERFLOW
#pragma push_macro("EILSEQ")
#undef EILSEQ
#pragma push_macro("EUSERS")
#undef EUSERS
#pragma push_macro("ENOTSOCK")
#undef ENOTSOCK
#pragma push_macro("EDESTADDRREQ")
#undef EDESTADDRREQ
#pragma push_macro("EMSGSIZE")
#undef EMSGSIZE
#pragma push_macro("EPROTOTYPE")
#undef EPROTOTYPE
#pragma push_macro("ENOPROTOOPT")
#undef ENOPROTOOPT
#pragma push_macro("EPROTONOSUPPORT")
#undef EPROTONOSUPPORT
#pragma push_macro("ESOCKTNOSUPPORT")
#undef ESOCKTNOSUPPORT
#pragma push_macro("EOPNOTSUPP")
#undef EOPNOTSUPP
#pragma push_macro("EPFNOSUPPORT")
#undef EPFNOSUPPORT
#pragma push_macro("EAFNOSUPPORT")
#undef EAFNOSUPPORT
#pragma push_macro("EADDRINUSE")
#undef EADDRINUSE
#pragma push_macro("EADDRNOTAVAIL")
#undef EADDRNOTAVAIL
#pragma push_macro("ENETDOWN")
#undef ENETDOWN
#pragma push_macro("ENETUNREACH")
#undef ENETUNREACH
#pragma push_macro("ENETRESET")
#undef ENETRESET
#pragma push_macro("ECONNABORTED")
#undef ECONNABORTED
#pragma push_macro("ECONNRESET")
#undef ECONNRESET
#pragma push_macro("ENOBUFS")
#undef ENOBUFS
#pragma push_macro("EISCONN")
#undef EISCONN
#pragma push_macro("ENOTCONN")
#undef ENOTCONN
#pragma push_macro("ESHUTDOWN")
#undef ESHUTDOWN
#pragma push_macro("ETOOMANYREFS")
#undef ETOOMANYREFS
#pragma push_macro("ETIMEDOUT")
#undef ETIMEDOUT
#pragma push_macro("ECONNREFUSED")
#undef ECONNREFUSED
#pragma push_macro("EHOSTDOWN")
#undef EHOSTDOWN
#pragma push_macro("EHOSTUNREACH")
#undef EHOSTUNREACH
#pragma push_macro("EALREADY")
#undef EALREADY
#pragma push_macro("EINPROGRESS")
#undef EINPROGRESS
#pragma push_macro("ESTALE")
#undef ESTALE
#pragma push_macro("EDQUOT")
#undef EDQUOT
#pragma push_macro("ECANCELED")
#undef ECANCELED
#pragma push_macro("EOWNERDEAD")
#undef EOWNERDEAD
#pragma push_macro("ENOTRECOVERABLE")
#undef ENOTRECOVERABLE
#pragma push_macro("EWOULDBLOCK")
#undef EWOULDBLOCK
#pragma push_macro("ENOTSUP")
#undef ENOTSUP
#include "core/linux_errno.h"
namespace venture::hle::libc::err {
inline constexpr int eperm = linux_errno::EPERM;
inline constexpr int enoent = linux_errno::ENOENT;
inline constexpr int esrch = linux_errno::ESRCH;
inline constexpr int eintr = linux_errno::EINTR;
inline constexpr int eio = linux_errno::EIO;
inline constexpr int enxio = linux_errno::ENXIO;
inline constexpr int e2big = linux_errno::E2BIG;
inline constexpr int enoexec = linux_errno::ENOEXEC;
inline constexpr int ebadf = linux_errno::EBADF;
inline constexpr int echild = linux_errno::ECHILD;
inline constexpr int eagain = linux_errno::EAGAIN;
inline constexpr int enomem = linux_errno::ENOMEM;
inline constexpr int eacces = linux_errno::EACCES;
inline constexpr int efault = linux_errno::EFAULT;
inline constexpr int enotblk = linux_errno::ENOTBLK;
inline constexpr int ebusy = linux_errno::EBUSY;
inline constexpr int eexist = linux_errno::EEXIST;
inline constexpr int exdev = linux_errno::EXDEV;
inline constexpr int enodev = linux_errno::ENODEV;
inline constexpr int enotdir = linux_errno::ENOTDIR;
inline constexpr int eisdir = linux_errno::EISDIR;
inline constexpr int einval = linux_errno::EINVAL;
inline constexpr int enfile = linux_errno::ENFILE;
inline constexpr int emfile = linux_errno::EMFILE;
inline constexpr int enotty = linux_errno::ENOTTY;
inline constexpr int etxtbsy = linux_errno::ETXTBSY;
inline constexpr int efbig = linux_errno::EFBIG;
inline constexpr int enospc = linux_errno::ENOSPC;
inline constexpr int espipe = linux_errno::ESPIPE;
inline constexpr int erofs = linux_errno::EROFS;
inline constexpr int emlink = linux_errno::EMLINK;
inline constexpr int epipe = linux_errno::EPIPE;
inline constexpr int edom = linux_errno::EDOM;
inline constexpr int erange = linux_errno::ERANGE;
inline constexpr int edeadlk = linux_errno::EDEADLK;
inline constexpr int enametoolong = linux_errno::ENAMETOOLONG;
inline constexpr int enolck = linux_errno::ENOLCK;
inline constexpr int enosys = linux_errno::ENOSYS;
inline constexpr int enotempty = linux_errno::ENOTEMPTY;
inline constexpr int eloop = linux_errno::ELOOP;
inline constexpr int enomsg = linux_errno::ENOMSG;
inline constexpr int eidrm = linux_errno::EIDRM;
inline constexpr int enostr = linux_errno::ENOSTR;
inline constexpr int enodata = linux_errno::ENODATA;
inline constexpr int etime = linux_errno::ETIME;
inline constexpr int enosr = linux_errno::ENOSR;
inline constexpr int eremote = linux_errno::EREMOTE;
inline constexpr int enolink = linux_errno::ENOLINK;
inline constexpr int eproto = linux_errno::EPROTO;
inline constexpr int emultihop = linux_errno::EMULTIHOP;
inline constexpr int ebadmsg = linux_errno::EBADMSG;
inline constexpr int eoverflow = linux_errno::EOVERFLOW;
inline constexpr int eilseq = linux_errno::EILSEQ;
inline constexpr int eusers = linux_errno::EUSERS;
inline constexpr int enotsock = linux_errno::ENOTSOCK;
inline constexpr int edestaddrreq = linux_errno::EDESTADDRREQ;
inline constexpr int emsgsize = linux_errno::EMSGSIZE;
inline constexpr int eprototype = linux_errno::EPROTOTYPE;
inline constexpr int enoprotoopt = linux_errno::ENOPROTOOPT;
inline constexpr int eprotonosupport = linux_errno::EPROTONOSUPPORT;
inline constexpr int esocktnosupport = linux_errno::ESOCKTNOSUPPORT;
inline constexpr int eopnotsupp = linux_errno::EOPNOTSUPP;
inline constexpr int epfnosupport = linux_errno::EPFNOSUPPORT;
inline constexpr int eafnosupport = linux_errno::EAFNOSUPPORT;
inline constexpr int eaddrinuse = linux_errno::EADDRINUSE;
inline constexpr int eaddrnotavail = linux_errno::EADDRNOTAVAIL;
inline constexpr int enetdown = linux_errno::ENETDOWN;
inline constexpr int enetunreach = linux_errno::ENETUNREACH;
inline constexpr int enetreset = linux_errno::ENETRESET;
inline constexpr int econnaborted = linux_errno::ECONNABORTED;
inline constexpr int econnreset = linux_errno::ECONNRESET;
inline constexpr int enobufs = linux_errno::ENOBUFS;
inline constexpr int eisconn = linux_errno::EISCONN;
inline constexpr int enotconn = linux_errno::ENOTCONN;
inline constexpr int eshutdown = linux_errno::ESHUTDOWN;
inline constexpr int etoomanyrefs = linux_errno::ETOOMANYREFS;
inline constexpr int etimedout = linux_errno::ETIMEDOUT;
inline constexpr int econnrefused = linux_errno::ECONNREFUSED;
inline constexpr int ehostdown = linux_errno::EHOSTDOWN;
inline constexpr int ehostunreach = linux_errno::EHOSTUNREACH;
inline constexpr int ealready = linux_errno::EALREADY;
inline constexpr int einprogress = linux_errno::EINPROGRESS;
inline constexpr int estale = linux_errno::ESTALE;
inline constexpr int edquot = linux_errno::EDQUOT;
inline constexpr int ecanceled = linux_errno::ECANCELED;
inline constexpr int eownerdead = linux_errno::EOWNERDEAD;
inline constexpr int enotrecoverable = linux_errno::ENOTRECOVERABLE;
inline constexpr int ewouldblock = linux_errno::EWOULDBLOCK;
inline constexpr int enotsup = linux_errno::ENOTSUP;
}  // namespace venture::hle::libc::err
#pragma pop_macro("EPERM")
#pragma pop_macro("ENOENT")
#pragma pop_macro("ESRCH")
#pragma pop_macro("EINTR")
#pragma pop_macro("EIO")
#pragma pop_macro("ENXIO")
#pragma pop_macro("E2BIG")
#pragma pop_macro("ENOEXEC")
#pragma pop_macro("EBADF")
#pragma pop_macro("ECHILD")
#pragma pop_macro("EAGAIN")
#pragma pop_macro("ENOMEM")
#pragma pop_macro("EACCES")
#pragma pop_macro("EFAULT")
#pragma pop_macro("ENOTBLK")
#pragma pop_macro("EBUSY")
#pragma pop_macro("EEXIST")
#pragma pop_macro("EXDEV")
#pragma pop_macro("ENODEV")
#pragma pop_macro("ENOTDIR")
#pragma pop_macro("EISDIR")
#pragma pop_macro("EINVAL")
#pragma pop_macro("ENFILE")
#pragma pop_macro("EMFILE")
#pragma pop_macro("ENOTTY")
#pragma pop_macro("ETXTBSY")
#pragma pop_macro("EFBIG")
#pragma pop_macro("ENOSPC")
#pragma pop_macro("ESPIPE")
#pragma pop_macro("EROFS")
#pragma pop_macro("EMLINK")
#pragma pop_macro("EPIPE")
#pragma pop_macro("EDOM")
#pragma pop_macro("ERANGE")
#pragma pop_macro("EDEADLK")
#pragma pop_macro("ENAMETOOLONG")
#pragma pop_macro("ENOLCK")
#pragma pop_macro("ENOSYS")
#pragma pop_macro("ENOTEMPTY")
#pragma pop_macro("ELOOP")
#pragma pop_macro("ENOMSG")
#pragma pop_macro("EIDRM")
#pragma pop_macro("ENOSTR")
#pragma pop_macro("ENODATA")
#pragma pop_macro("ETIME")
#pragma pop_macro("ENOSR")
#pragma pop_macro("EREMOTE")
#pragma pop_macro("ENOLINK")
#pragma pop_macro("EPROTO")
#pragma pop_macro("EMULTIHOP")
#pragma pop_macro("EBADMSG")
#pragma pop_macro("EOVERFLOW")
#pragma pop_macro("EILSEQ")
#pragma pop_macro("EUSERS")
#pragma pop_macro("ENOTSOCK")
#pragma pop_macro("EDESTADDRREQ")
#pragma pop_macro("EMSGSIZE")
#pragma pop_macro("EPROTOTYPE")
#pragma pop_macro("ENOPROTOOPT")
#pragma pop_macro("EPROTONOSUPPORT")
#pragma pop_macro("ESOCKTNOSUPPORT")
#pragma pop_macro("EOPNOTSUPP")
#pragma pop_macro("EPFNOSUPPORT")
#pragma pop_macro("EAFNOSUPPORT")
#pragma pop_macro("EADDRINUSE")
#pragma pop_macro("EADDRNOTAVAIL")
#pragma pop_macro("ENETDOWN")
#pragma pop_macro("ENETUNREACH")
#pragma pop_macro("ENETRESET")
#pragma pop_macro("ECONNABORTED")
#pragma pop_macro("ECONNRESET")
#pragma pop_macro("ENOBUFS")
#pragma pop_macro("EISCONN")
#pragma pop_macro("ENOTCONN")
#pragma pop_macro("ESHUTDOWN")
#pragma pop_macro("ETOOMANYREFS")
#pragma pop_macro("ETIMEDOUT")
#pragma pop_macro("ECONNREFUSED")
#pragma pop_macro("EHOSTDOWN")
#pragma pop_macro("EHOSTUNREACH")
#pragma pop_macro("EALREADY")
#pragma pop_macro("EINPROGRESS")
#pragma pop_macro("ESTALE")
#pragma pop_macro("EDQUOT")
#pragma pop_macro("ECANCELED")
#pragma pop_macro("EOWNERDEAD")
#pragma pop_macro("ENOTRECOVERABLE")
#pragma pop_macro("EWOULDBLOCK")
#pragma pop_macro("ENOTSUP")
