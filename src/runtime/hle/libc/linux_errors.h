// Preserve host errno macros while importing the core Linux values.
#pragma once
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
namespace venture::hle::err {
inline constexpr int kEPERM = linux_errno::EPERM;
inline constexpr int kENOENT = linux_errno::ENOENT;
inline constexpr int kESRCH = linux_errno::ESRCH;
inline constexpr int kEINTR = linux_errno::EINTR;
inline constexpr int kEIO = linux_errno::EIO;
inline constexpr int kENXIO = linux_errno::ENXIO;
inline constexpr int kE2BIG = linux_errno::E2BIG;
inline constexpr int kENOEXEC = linux_errno::ENOEXEC;
inline constexpr int kEBADF = linux_errno::EBADF;
inline constexpr int kECHILD = linux_errno::ECHILD;
inline constexpr int kEAGAIN = linux_errno::EAGAIN;
inline constexpr int kENOMEM = linux_errno::ENOMEM;
inline constexpr int kEACCES = linux_errno::EACCES;
inline constexpr int kEFAULT = linux_errno::EFAULT;
inline constexpr int kENOTBLK = linux_errno::ENOTBLK;
inline constexpr int kEBUSY = linux_errno::EBUSY;
inline constexpr int kEEXIST = linux_errno::EEXIST;
inline constexpr int kEXDEV = linux_errno::EXDEV;
inline constexpr int kENODEV = linux_errno::ENODEV;
inline constexpr int kENOTDIR = linux_errno::ENOTDIR;
inline constexpr int kEISDIR = linux_errno::EISDIR;
inline constexpr int kEINVAL = linux_errno::EINVAL;
inline constexpr int kENFILE = linux_errno::ENFILE;
inline constexpr int kEMFILE = linux_errno::EMFILE;
inline constexpr int kENOTTY = linux_errno::ENOTTY;
inline constexpr int kETXTBSY = linux_errno::ETXTBSY;
inline constexpr int kEFBIG = linux_errno::EFBIG;
inline constexpr int kENOSPC = linux_errno::ENOSPC;
inline constexpr int kESPIPE = linux_errno::ESPIPE;
inline constexpr int kEROFS = linux_errno::EROFS;
inline constexpr int kEMLINK = linux_errno::EMLINK;
inline constexpr int kEPIPE = linux_errno::EPIPE;
inline constexpr int kEDOM = linux_errno::EDOM;
inline constexpr int kERANGE = linux_errno::ERANGE;
inline constexpr int kEDEADLK = linux_errno::EDEADLK;
inline constexpr int kENAMETOOLONG = linux_errno::ENAMETOOLONG;
inline constexpr int kENOLCK = linux_errno::ENOLCK;
inline constexpr int kENOSYS = linux_errno::ENOSYS;
inline constexpr int kENOTEMPTY = linux_errno::ENOTEMPTY;
inline constexpr int kELOOP = linux_errno::ELOOP;
inline constexpr int kENOMSG = linux_errno::ENOMSG;
inline constexpr int kEIDRM = linux_errno::EIDRM;
inline constexpr int kENOSTR = linux_errno::ENOSTR;
inline constexpr int kENODATA = linux_errno::ENODATA;
inline constexpr int kETIME = linux_errno::ETIME;
inline constexpr int kENOSR = linux_errno::ENOSR;
inline constexpr int kEREMOTE = linux_errno::EREMOTE;
inline constexpr int kENOLINK = linux_errno::ENOLINK;
inline constexpr int kEPROTO = linux_errno::EPROTO;
inline constexpr int kEMULTIHOP = linux_errno::EMULTIHOP;
inline constexpr int kEBADMSG = linux_errno::EBADMSG;
inline constexpr int kEOVERFLOW = linux_errno::EOVERFLOW;
inline constexpr int kEILSEQ = linux_errno::EILSEQ;
inline constexpr int kEUSERS = linux_errno::EUSERS;
inline constexpr int kENOTSOCK = linux_errno::ENOTSOCK;
inline constexpr int kEDESTADDRREQ = linux_errno::EDESTADDRREQ;
inline constexpr int kEMSGSIZE = linux_errno::EMSGSIZE;
inline constexpr int kEPROTOTYPE = linux_errno::EPROTOTYPE;
inline constexpr int kENOPROTOOPT = linux_errno::ENOPROTOOPT;
inline constexpr int kEPROTONOSUPPORT = linux_errno::EPROTONOSUPPORT;
inline constexpr int kESOCKTNOSUPPORT = linux_errno::ESOCKTNOSUPPORT;
inline constexpr int kEOPNOTSUPP = linux_errno::EOPNOTSUPP;
inline constexpr int kEPFNOSUPPORT = linux_errno::EPFNOSUPPORT;
inline constexpr int kEAFNOSUPPORT = linux_errno::EAFNOSUPPORT;
inline constexpr int kEADDRINUSE = linux_errno::EADDRINUSE;
inline constexpr int kEADDRNOTAVAIL = linux_errno::EADDRNOTAVAIL;
inline constexpr int kENETDOWN = linux_errno::ENETDOWN;
inline constexpr int kENETUNREACH = linux_errno::ENETUNREACH;
inline constexpr int kENETRESET = linux_errno::ENETRESET;
inline constexpr int kECONNABORTED = linux_errno::ECONNABORTED;
inline constexpr int kECONNRESET = linux_errno::ECONNRESET;
inline constexpr int kENOBUFS = linux_errno::ENOBUFS;
inline constexpr int kEISCONN = linux_errno::EISCONN;
inline constexpr int kENOTCONN = linux_errno::ENOTCONN;
inline constexpr int kESHUTDOWN = linux_errno::ESHUTDOWN;
inline constexpr int kETOOMANYREFS = linux_errno::ETOOMANYREFS;
inline constexpr int kETIMEDOUT = linux_errno::ETIMEDOUT;
inline constexpr int kECONNREFUSED = linux_errno::ECONNREFUSED;
inline constexpr int kEHOSTDOWN = linux_errno::EHOSTDOWN;
inline constexpr int kEHOSTUNREACH = linux_errno::EHOSTUNREACH;
inline constexpr int kEALREADY = linux_errno::EALREADY;
inline constexpr int kEINPROGRESS = linux_errno::EINPROGRESS;
inline constexpr int kESTALE = linux_errno::ESTALE;
inline constexpr int kEDQUOT = linux_errno::EDQUOT;
inline constexpr int kECANCELED = linux_errno::ECANCELED;
inline constexpr int kEOWNERDEAD = linux_errno::EOWNERDEAD;
inline constexpr int kENOTRECOVERABLE = linux_errno::ENOTRECOVERABLE;
inline constexpr int kEWOULDBLOCK = linux_errno::EWOULDBLOCK;
inline constexpr int kENOTSUP = linux_errno::ENOTSUP;
}  // namespace venture::hle::err
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
