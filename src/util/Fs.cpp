// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Logging.h"
#include "crypto/Hex.h"
#include "lib/util/format.h"
#include <regex>
#include <map>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include <cstdio>

namespace stellar
{

namespace fs
{

#ifdef _WIN32
#include <Windows.h>
#include <Shellapi.h>
#include <psapi.h>

static std::map<std::string, HANDLE> lockMap;

bool
lockFile(std::string const& path)
{
    if (lockMap.find(path) != lockMap.end())
    {
        throw std::runtime_error("file is already locked by this process");
    }
    HANDLE h = ::CreateFile(path.c_str(), GENERIC_WRITE,
                            0, // don't allow sharing
                            NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY |
                                FILE_FLAG_DELETE_ON_CLOSE,
                            NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        lockMap.insert(std::make_pair(path, h));
    }
    return h != INVALID_HANDLE_VALUE;
}

void
unlockFile(std::string const& path)
{
    auto it = lockMap.find(path);
    if (it != lockMap.end())
    {
        ::CloseHandle(it->second);
        lockMap.erase(it);
    }
    else
    {
        throw std::runtime_error("file was not locked");
    }
}

bool
exists(std::string const& name)
{
    if (name.empty())
        return false;

    if (GetFileAttributes(name.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            return false;
        }
        else
        {
            std::string msg("error accessing path: ");
            throw std::runtime_error(msg + name);
        }
    }
    return true;
}

bool
mkdir(std::string const& name)
{
    bool b = _mkdir(name.c_str()) == 0;
    CLOG(DEBUG, "Fs") << (b ? "created dir " : "failed to create dir ") << name;
    return b;
}

void
deltree(std::string const& d)
{
    SHFILEOPSTRUCT s = {0};
    std::string from = d;
    from.push_back('\0');
    from.push_back('\0');
    s.wFunc = FO_DELETE;
    s.pFrom = from.data();
    s.fFlags = FOF_NO_UI;
    if (SHFileOperation(&s) != 0)
    {
        throw std::runtime_error("SHFileOperation failed in deltree");
    }
}

long
getCurrentPid()
{
    return static_cast<long>(GetCurrentProcessId());
}

bool
processExists(long pid)
{
    std::vector<DWORD> buffer(4096);
    DWORD bytesWritten;
    for (;;)
    {
        if (!EnumProcesses(buffer.data(),
                           static_cast<DWORD>(buffer.size() * sizeof(DWORD)),
                           &bytesWritten))
        {
            throw std::runtime_error("EnumProcess failed");
        }
        if (bytesWritten / sizeof(DWORD) < buffer.size())
        {
            auto found = std::find(buffer.begin(), buffer.end(), pid);
            return !(found == buffer.end());
        }
        // Need a larger buffer to hold all the ids.
        buffer.resize(buffer.size() * 2);
    }
}

#else
#include <ftw.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/types.h>
#include <fcntl.h>
#include <cerrno>

static std::map<std::string, int> lockMap;

bool
lockFile(std::string const& path)
{
    if (lockMap.find(path) != lockMap.end())
    {
        throw std::runtime_error("file is already locked by this process");
    }
    int fd = open(path.c_str(), O_RDWR | O_CREAT, S_IRWXU);

    if (fd != -1)
    {
        int r = flock(fd, LOCK_EX | LOCK_NB);
        if (r == 0)
        {
            lockMap.insert(std::make_pair(path, fd));
        }
        else
        {
            close(fd);
            fd = -1;
        }
    }
    return fd != -1;
}

void
unlockFile(std::string const& path)
{
    auto it = lockMap.find(path);
    if (it != lockMap.end())
    {
        // cannot unlink to avoid potential race
        close(it->second);
        lockMap.erase(it);
    }
    else
    {
        throw std::runtime_error("file was not locked");
    }
}

bool
exists(std::string const& name)
{
    struct stat buf;
    if (stat(name.c_str(), &buf) == -1)
    {
        if (errno == ENOENT)
        {
            return false;
        }
        else
        {
            std::string msg("error accessing path: ");
            throw std::runtime_error(msg + name);
        }
    }
    return true;
}

bool
mkdir(std::string const& name)
{
    bool b = ::mkdir(name.c_str(), 0700) == 0;
    CLOG(DEBUG, "Fs") << (b ? "created dir " : "failed to create dir ") << name;
    return b;
}

int
callback(char const* name, struct stat const* st, int flag, struct FTW* ftw)
{
    CLOG(DEBUG, "Fs") << "deleting: " << name;
    if (flag == FTW_DP)
    {
        if (rmdir(name) != 0)
        {
            throw std::runtime_error("rmdir failed");
        }
    }
    else
    {
        if (std::remove(name) != 0)
        {
            throw std::runtime_error("std::remove failed");
        }
    }
    return 0;
}

void
deltree(std::string const& d)
{
    if (nftw(d.c_str(), callback, FOPEN_MAX, FTW_DEPTH) != 0)
    {
        throw std::runtime_error("nftw failed in deltree");
    }
}

long
getCurrentPid()
{
    return static_cast<long>(getpid());
}

bool
processExists(long pid)
{
    return (kill(pid, 0) == 0);
}

#endif

bool
mkpath(const std::string &path)
{
    auto pos = 0;
    while (pos < path.length())
    {
        auto slash = path.find('/', pos);
        pos = slash == std::string::npos ? path.length() : slash;
        if (!exists(path.substr(0, pos)) && !mkdir(path.substr(0, pos)))
        {
            return false;
        }
        pos++;
    }

    return true;
}

std::string
hexStr(uint32_t checkpointNum)
{
    return fmt::format("{:08x}", checkpointNum);
}

std::string
hexDir(std::string const& hexStr)
{
    std::regex rx("([[:xdigit:]]{2})([[:xdigit:]]{2})([[:xdigit:]]{2}).*");
    std::smatch sm;
    bool matched = std::regex_match(hexStr, sm, rx);
    assert(matched);
    return (std::string(sm[1]) + "/" + std::string(sm[2]) + "/" +
            std::string(sm[3]));
}

std::string
baseName(std::string const& type, std::string const& hexStr,
         std::string const& suffix)
{
    return type + "-" + hexStr + "." + suffix;
}

std::string
remoteDir(std::string const& type, std::string const& hexStr)
{
    return type + "/" + hexDir(hexStr);
}

std::string
remoteName(std::string const& type, std::string const& hexStr,
           std::string const& suffix)
{
    return remoteDir(type, hexStr) + "/" + baseName(type, hexStr, suffix);
}
}
}
