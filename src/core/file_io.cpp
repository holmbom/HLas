#include "file_io.h"

#ifdef _WIN32
#include "path_utf8.h"
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace HLas::detail {

MappedFile::~MappedFile() {
    close();
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(other.data_)
    , size_(other.size_)
#ifdef _WIN32
    , file_handle_(other.file_handle_)
    , mapping_handle_(other.mapping_handle_)
#else
    , fd_(other.fd_)
#endif
{
    other.data_ = nullptr;
    other.size_ = 0;
#ifdef _WIN32
    other.file_handle_ = nullptr;
    other.mapping_handle_ = nullptr;
#else
    other.fd_ = -1;
#endif
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        data_ = other.data_;
        size_ = other.size_;
#ifdef _WIN32
        file_handle_ = other.file_handle_;
        mapping_handle_ = other.mapping_handle_;
        other.file_handle_ = nullptr;
        other.mapping_handle_ = nullptr;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

#ifdef _WIN32

ErrorCode MappedFile::open(const char* path) {
    close();

    const std::wstring wpath = utf8_to_wide(path);
    if (wpath.empty())
        return ErrorCode::FileNotFound;   // ogiltig UTF-8

    file_handle_ = CreateFileW(
        wpath.c_str(),
        GENERIC_READ,
        // ⛔ FILE_SHARE_DELETE hör hit, och det är inte en detalj. Utan den
        // vägrar Windows radera eller byta namn på en fil så länge vi har den
        // mappad, medan POSIX tillåter det och låter mappningen leva vidare på
        // det gamla innehållet. Två följder av att sakna den:
        //   - BuildSidecar() misslyckades med FileOpenFailed om NÅGON hade den
        //     gamla .hlz-filen öppen, eftersom MoveFileExW inte fick ersätta
        //     den. På Linux lyckas samma anrop.
        //   - Testerna kunde inte städa efter sig; filerna låg kvar i TEMP.
        // Med flaggan beter sig de två plattformarna lika: raderingen går
        // igenom, och vår mappning fortsätter läsa de byte den redan pekar på
        // tills läsaren stängs.
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (file_handle_ == INVALID_HANDLE_VALUE) {
        file_handle_ = nullptr;
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return ErrorCode::FileNotFound;
        return ErrorCode::FileOpenFailed;
    }

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file_handle_, &file_size)) {
        close();
        return ErrorCode::FileOpenFailed;
    }
    size_ = static_cast<size_t>(file_size.QuadPart);

    if (size_ == 0) {
        close();
        return ErrorCode::FileOpenFailed;
    }

    mapping_handle_ = CreateFileMappingW(
        file_handle_,
        nullptr,
        PAGE_READONLY,
        0, 0,
        nullptr
    );

    if (!mapping_handle_) {
        close();
        return ErrorCode::MmapFailed;
    }

    data_ = static_cast<const uint8_t*>(
        MapViewOfFile(mapping_handle_, FILE_MAP_READ, 0, 0, 0)
    );

    if (!data_) {
        close();
        return ErrorCode::MmapFailed;
    }

    return ErrorCode::Ok;
}

void MappedFile::close() {
    if (data_) {
        UnmapViewOfFile(data_);
        data_ = nullptr;
    }
    if (mapping_handle_) {
        CloseHandle(mapping_handle_);
        mapping_handle_ = nullptr;
    }
    if (file_handle_) {
        CloseHandle(file_handle_);
        file_handle_ = nullptr;
    }
    size_ = 0;
}

#else // Linux / macOS

ErrorCode MappedFile::open(const char* path) {
    close();

    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) {
        if (errno == ENOENT)
            return ErrorCode::FileNotFound;
        return ErrorCode::FileOpenFailed;
    }

    struct stat st;
    if (fstat(fd_, &st) != 0) {
        close();
        return ErrorCode::FileOpenFailed;
    }
    size_ = static_cast<size_t>(st.st_size);

    if (size_ == 0) {
        close();
        return ErrorCode::FileOpenFailed;
    }

    void* ptr = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (ptr == MAP_FAILED) {
        close();
        return ErrorCode::MmapFailed;
    }
    data_ = static_cast<const uint8_t*>(ptr);

    madvise(const_cast<uint8_t*>(data_), size_, MADV_SEQUENTIAL);

    return ErrorCode::Ok;
}

void MappedFile::close() {
    if (data_) {
        munmap(const_cast<uint8_t*>(data_), size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    size_ = 0;
}

#endif

} // namespace HLas::detail
