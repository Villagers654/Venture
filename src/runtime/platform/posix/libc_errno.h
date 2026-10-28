#pragma once
#include <cerrno>
namespace venture::hle::libc::host {
inline int native_error(int value = errno) {
  switch (value) {
    case 0:
      return 0;
#ifdef EPERM
    case EPERM:
      return -err::eperm;
#endif
#ifdef ENOENT
    case ENOENT:
      return -err::enoent;
#endif
#ifdef ESRCH
    case ESRCH:
      return -err::esrch;
#endif
#ifdef EINTR
    case EINTR:
      return -err::eintr;
#endif
#ifdef EIO
    case EIO:
      return -err::eio;
#endif
#ifdef ENXIO
    case ENXIO:
      return -err::enxio;
#endif
#ifdef E2BIG
    case E2BIG:
      return -err::e2big;
#endif
#ifdef ENOEXEC
    case ENOEXEC:
      return -err::enoexec;
#endif
#ifdef EBADF
    case EBADF:
      return -err::ebadf;
#endif
#ifdef ECHILD
    case ECHILD:
      return -err::echild;
#endif
#ifdef EAGAIN
    case EAGAIN:
      return -err::eagain;
#endif
#ifdef ENOMEM
    case ENOMEM:
      return -err::enomem;
#endif
#ifdef EACCES
    case EACCES:
      return -err::eacces;
#endif
#ifdef EFAULT
    case EFAULT:
      return -err::efault;
#endif
#ifdef ENOTBLK
    case ENOTBLK:
      return -err::enotblk;
#endif
#ifdef EBUSY
    case EBUSY:
      return -err::ebusy;
#endif
#ifdef EEXIST
    case EEXIST:
      return -err::eexist;
#endif
#ifdef EXDEV
    case EXDEV:
      return -err::exdev;
#endif
#ifdef ENODEV
    case ENODEV:
      return -err::enodev;
#endif
#ifdef ENOTDIR
    case ENOTDIR:
      return -err::enotdir;
#endif
#ifdef EISDIR
    case EISDIR:
      return -err::eisdir;
#endif
#ifdef EINVAL
    case EINVAL:
      return -err::einval;
#endif
#ifdef ENFILE
    case ENFILE:
      return -err::enfile;
#endif
#ifdef EMFILE
    case EMFILE:
      return -err::emfile;
#endif
#ifdef ENOTTY
    case ENOTTY:
      return -err::enotty;
#endif
#ifdef ETXTBSY
    case ETXTBSY:
      return -err::etxtbsy;
#endif
#ifdef EFBIG
    case EFBIG:
      return -err::efbig;
#endif
#ifdef ENOSPC
    case ENOSPC:
      return -err::enospc;
#endif
#ifdef ESPIPE
    case ESPIPE:
      return -err::espipe;
#endif
#ifdef EROFS
    case EROFS:
      return -err::erofs;
#endif
#ifdef EMLINK
    case EMLINK:
      return -err::emlink;
#endif
#ifdef EPIPE
    case EPIPE:
      return -err::epipe;
#endif
#ifdef EDOM
    case EDOM:
      return -err::edom;
#endif
#ifdef ERANGE
    case ERANGE:
      return -err::erange;
#endif
#ifdef EDEADLK
    case EDEADLK:
      return -err::edeadlk;
#endif
#ifdef ENAMETOOLONG
    case ENAMETOOLONG:
      return -err::enametoolong;
#endif
#ifdef ENOLCK
    case ENOLCK:
      return -err::enolck;
#endif
#ifdef ENOSYS
    case ENOSYS:
      return -err::enosys;
#endif
#ifdef ENOTEMPTY
    case ENOTEMPTY:
      return -err::enotempty;
#endif
#ifdef ELOOP
    case ELOOP:
      return -err::eloop;
#endif
#ifdef ENOMSG
    case ENOMSG:
      return -err::enomsg;
#endif
#ifdef EIDRM
    case EIDRM:
      return -err::eidrm;
#endif
#ifdef ENOSTR
    case ENOSTR:
      return -err::enostr;
#endif
#ifdef ENODATA
    case ENODATA:
      return -err::enodata;
#endif
#ifdef ETIME
    case ETIME:
      return -err::etime;
#endif
#ifdef ENOSR
    case ENOSR:
      return -err::enosr;
#endif
#ifdef EREMOTE
    case EREMOTE:
      return -err::eremote;
#endif
#ifdef ENOLINK
    case ENOLINK:
      return -err::enolink;
#endif
#ifdef EPROTO
    case EPROTO:
      return -err::eproto;
#endif
#ifdef EMULTIHOP
    case EMULTIHOP:
      return -err::emultihop;
#endif
#ifdef EBADMSG
    case EBADMSG:
      return -err::ebadmsg;
#endif
#ifdef EOVERFLOW
    case EOVERFLOW:
      return -err::eoverflow;
#endif
#ifdef EILSEQ
    case EILSEQ:
      return -err::eilseq;
#endif
#ifdef EUSERS
    case EUSERS:
      return -err::eusers;
#endif
#ifdef ENOTSOCK
    case ENOTSOCK:
      return -err::enotsock;
#endif
#ifdef EDESTADDRREQ
    case EDESTADDRREQ:
      return -err::edestaddrreq;
#endif
#ifdef EMSGSIZE
    case EMSGSIZE:
      return -err::emsgsize;
#endif
#ifdef EPROTOTYPE
    case EPROTOTYPE:
      return -err::eprototype;
#endif
#ifdef ENOPROTOOPT
    case ENOPROTOOPT:
      return -err::enoprotoopt;
#endif
#ifdef EPROTONOSUPPORT
    case EPROTONOSUPPORT:
      return -err::eprotonosupport;
#endif
#ifdef ESOCKTNOSUPPORT
    case ESOCKTNOSUPPORT:
      return -err::esocktnosupport;
#endif
#ifdef EOPNOTSUPP
    case EOPNOTSUPP:
      return -err::eopnotsupp;
#endif
#ifdef EPFNOSUPPORT
    case EPFNOSUPPORT:
      return -err::epfnosupport;
#endif
#ifdef EAFNOSUPPORT
    case EAFNOSUPPORT:
      return -err::eafnosupport;
#endif
#ifdef EADDRINUSE
    case EADDRINUSE:
      return -err::eaddrinuse;
#endif
#ifdef EADDRNOTAVAIL
    case EADDRNOTAVAIL:
      return -err::eaddrnotavail;
#endif
#ifdef ENETDOWN
    case ENETDOWN:
      return -err::enetdown;
#endif
#ifdef ENETUNREACH
    case ENETUNREACH:
      return -err::enetunreach;
#endif
#ifdef ENETRESET
    case ENETRESET:
      return -err::enetreset;
#endif
#ifdef ECONNABORTED
    case ECONNABORTED:
      return -err::econnaborted;
#endif
#ifdef ECONNRESET
    case ECONNRESET:
      return -err::econnreset;
#endif
#ifdef ENOBUFS
    case ENOBUFS:
      return -err::enobufs;
#endif
#ifdef EISCONN
    case EISCONN:
      return -err::eisconn;
#endif
#ifdef ENOTCONN
    case ENOTCONN:
      return -err::enotconn;
#endif
#ifdef ESHUTDOWN
    case ESHUTDOWN:
      return -err::eshutdown;
#endif
#ifdef ETOOMANYREFS
    case ETOOMANYREFS:
      return -err::etoomanyrefs;
#endif
#ifdef ETIMEDOUT
    case ETIMEDOUT:
      return -err::etimedout;
#endif
#ifdef ECONNREFUSED
    case ECONNREFUSED:
      return -err::econnrefused;
#endif
#ifdef EHOSTDOWN
    case EHOSTDOWN:
      return -err::ehostdown;
#endif
#ifdef EHOSTUNREACH
    case EHOSTUNREACH:
      return -err::ehostunreach;
#endif
#ifdef EALREADY
    case EALREADY:
      return -err::ealready;
#endif
#ifdef EINPROGRESS
    case EINPROGRESS:
      return -err::einprogress;
#endif
#ifdef ESTALE
    case ESTALE:
      return -err::estale;
#endif
#ifdef EDQUOT
    case EDQUOT:
      return -err::edquot;
#endif
#ifdef ECANCELED
    case ECANCELED:
      return -err::ecanceled;
#endif
#ifdef EOWNERDEAD
    case EOWNERDEAD:
      return -err::eownerdead;
#endif
#ifdef ENOTRECOVERABLE
    case ENOTRECOVERABLE:
      return -err::enotrecoverable;
#endif
    default:
      return -err::eio;
  }
}
}  // namespace venture::hle::libc::host
