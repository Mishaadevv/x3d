#include "Core/Result.h"

namespace zx {

const char* ErrorCodeName(ErrorCode code)
{
    switch (code) {
    case ErrorCode::Unknown:
        return "Unknown";
    case ErrorCode::NotFound:
        return "NotFound";
    case ErrorCode::AccessDenied:
        return "AccessDenied";
    case ErrorCode::InvalidData:
        return "InvalidData";
    case ErrorCode::Unsupported:
        return "Unsupported";
    case ErrorCode::OutOfMemory:
        return "OutOfMemory";
    case ErrorCode::Timeout:
        return "Timeout";
    case ErrorCode::Cancelled:
        return "Cancelled";
    case ErrorCode::AlreadyExists:
        return "AlreadyExists";
    case ErrorCode::InvalidArgument:
        return "InvalidArgument";
    case ErrorCode::DeviceLost:
        return "DeviceLost";
    case ErrorCode::Internal:
        return "Internal";
    }
    return "?";
}

} // namespace zx
