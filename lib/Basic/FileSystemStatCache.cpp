//===--- FileSystemStatCache.cpp - Caching for 'stat' calls ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file defines the FileSystemStatCache interface.
//
//===----------------------------------------------------------------------===//

#include "clang/Basic/FileSystemStatCache.h"
#include "clang/Basic/VirtualFileSystem.h"
#include "llvm/Support/Path.h"

// FIXME: This is terrible, we need this for ::close.
#if !defined(_MSC_VER) && !defined(__MINGW32__)
#include <unistd.h>
#include <sys/uio.h>
#else
#include <io.h>
#endif
using namespace clang;

#if defined(_MSC_VER)
#define S_ISDIR(s) ((_S_IFDIR & s) !=0)
#endif

void FileSystemStatCache::anchor() { }

static void copyStatusToFileData(const vfs::Status &Status,
                                 FileData &Data) {
  Data.Name = Status.getName();
  Data.Size = Status.getSize();
  Data.ModTime = Status.getLastModificationTime().toEpochTime();
  Data.UniqueID = Status.getUniqueID();
  Data.IsDirectory = Status.isDirectory();
  Data.IsNamedPipe = Status.getType() == llvm::sys::fs::file_type::fifo_file;
  Data.InPCH = false;
}

/// FileSystemStatCache::get - Get the 'stat' information for the specified
/// path, using the cache to accelerate it if possible.  This returns true if
/// the path does not exist or false if it exists.
///
/// If isFile is true, then this lookup should only return success for files
/// (not directories).  If it is false this lookup should only return
/// success for directories (not files).  On a successful file lookup, the
/// implementation can optionally fill in FileDescriptor with a valid
/// descriptor and the client guarantees that it will close it.
bool FileSystemStatCache::get(const char *Path, FileData &Data, bool isFile,
                              vfs::File **F, FileSystemStatCache *Cache,
                              vfs::FileSystem &FS) {
  LookupResult R;
  bool isForDir = !isFile;

  // If we have a cache, use it to resolve the stat query.
  if (Cache)
    R = Cache->getStat(Path, Data, isFile, F, FS);
  else if (isForDir || !F) {
    // If this is a directory or a file descriptor is not needed and we have
    // no cache, just go to the file system.
    llvm::ErrorOr<vfs::Status> Status = FS.status(Path);
    if (!Status) {
      R = CacheMissing;
    } else {
      R = CacheExists;
      copyStatusToFileData(*Status, Data);
    }
  } else {
    // Otherwise, we have to go to the filesystem.  We can always just use
    // 'stat' here, but (for files) the client is asking whether the file exists
    // because it wants to turn around and *open* it.  It is more efficient to
    // do "open+fstat" on success than it is to do "stat+open".
    //
    // Because of this, check to see if the file exists with 'open'.  If the
    // open succeeds, use fstat to get the stat info.
    std::unique_ptr<vfs::File> OwnedFile;
    llvm::error_code EC = FS.openFileForRead(Path, OwnedFile);

    if (EC) {
      // If the open fails, our "stat" fails.
      R = CacheMissing;
    } else {
      // Otherwise, the open succeeded.  Do an fstat to get the information
      // about the file.  We'll end up returning the open file descriptor to the
      // client to do what they please with it.
      llvm::ErrorOr<vfs::Status> Status = OwnedFile->status();
      if (Status) {
        R = CacheExists;
        copyStatusToFileData(*Status, Data);
        *F = OwnedFile.release();
      } else {
        // fstat rarely fails.  If it does, claim the initial open didn't
        // succeed.
        R = CacheMissing;
        *F = 0;
      }
    }
  }

  // If the path doesn't exist, return failure.
  if (R == CacheMissing) return true;
  
  // If the path exists, make sure that its "directoryness" matches the clients
  // demands.
  if (Data.IsDirectory != isForDir) {
    // If not, close the file if opened.
    if (F && *F) {
      (*F)->close();
      *F = 0;
    }
    
    return true;
  }
  
  return false;
}

MemorizeStatCalls::LookupResult
MemorizeStatCalls::getStat(const char *Path, FileData &Data, bool isFile,
                           vfs::File **F, vfs::FileSystem &FS) {
  LookupResult Result = statChained(Path, Data, isFile, F, FS);

  // Do not cache failed stats, it is easy to construct common inconsistent
  // situations if we do, and they are not important for PCH performance (which
  // currently only needs the stats to construct the initial FileManager
  // entries).
  if (Result == CacheMissing)
    return Result;
  
  // Cache file 'stat' results and directories with absolutely paths.
  if (!Data.IsDirectory || llvm::sys::path::is_absolute(Path))
    StatCalls[Path] = Data;

  return Result;
}
