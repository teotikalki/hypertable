/**
 * Copyright (C) 2007 Doug Judd (Zvents, Inc.)
 *
 * This file is part of Hypertable.
 *
 * Hypertable is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * Hypertable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "Common/Compat.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
}

#include "Common/String.h"
#include "Common/FileUtils.h"
#include "Common/System.h"

#include "LocalBroker.h"

using namespace Hypertable;


LocalBroker::LocalBroker(PropertiesPtr &props) : m_verbose(false) {
  const char *root;

  m_verbose = props->get_bool("Hypertable.Verbose", false);

  /**
   * Determine root directory
   */
  if ((root = props->get("DfsBroker.Local.Root", 0)) == 0) {
    HT_ERROR("Required property 'DfsBroker.Local.Root' not found, exiting...");
    exit(1);
  }

  m_rootdir = (root[0] == '/') ? root : System::install_dir + "/" + root;

  // strip off the trailing '/'
  if (root[strlen(root)-1] == '/')
    m_rootdir = m_rootdir.substr(0, m_rootdir.length()-1);

  // ensure that root directory exists
  if (!FileUtils::mkdirs(m_rootdir))
    exit(1);
}



LocalBroker::~LocalBroker() {
}


/**
 * Open
 */
void
LocalBroker::open(ResponseCallbackOpen *cb, const char *fname, uint32_t bufsz) {
  int fd;
  String abspath;

  if (m_verbose) {
    HT_INFOF("open file='%s' bufsz=%d", fname, bufsz);
  }

  if (fname[0] == '/')
    abspath = m_rootdir + fname;
  else
    abspath = m_rootdir + "/" + fname;

  //fd = atomic_inc_return(&ms_unique_id);

  /**
   * Open the file
   */
  if ((fd = ::open(abspath.c_str(), O_RDONLY)) == -1) {
    HT_ERRORF("open failed: file='%s' - %s", abspath.c_str(), strerror(errno));
    report_error(cb);
    return;
  }

  {
    struct sockaddr_in addr;
    OpenFileDataLocalPtr fdata(new OpenFileDataLocal(fd, O_RDONLY));

    cb->get_address(addr);

    m_open_file_map.create(fd, addr, fdata);

    cb->response(fd);
  }
}


/**
 * Create
 */
void
LocalBroker::create(ResponseCallbackOpen *cb, const char *fname, bool overwrite,
                    uint32_t bufsz, uint16_t replication, uint64_t blksz) {
  int fd;
  int flags;
  String abspath;

  if (m_verbose) {
    HT_INFOF("create file='%s' overwrite=%d bufsz=%d replication=%d blksz=%d",
                fname, (int)overwrite, bufsz, replication, blksz);
  }

  if (fname[0] == '/')
    abspath = m_rootdir + fname;
  else
    abspath = m_rootdir + "/" + fname;

  //fd = atomic_inc_return(&ms_unique_id);

  if (overwrite)
    flags = O_WRONLY | O_CREAT | O_TRUNC;
  else
    flags = O_WRONLY | O_CREAT | O_APPEND;

  /**
   * Open the file
   */
  if ((fd = ::open(abspath.c_str(), flags, 0644)) == -1) {
    HT_ERRORF("open failed: file='%s' - %s", abspath.c_str(), strerror(errno));
    report_error(cb);
    return;
  }

  {
    struct sockaddr_in addr;
    OpenFileDataLocalPtr fdata(new OpenFileDataLocal(fd, O_WRONLY));

    cb->get_address(addr);

    m_open_file_map.create(fd, addr, fdata);

    cb->response(fd);
  }
}


/**
 * Close
 */
void LocalBroker::close(ResponseCallback *cb, uint32_t fd) {
  if (m_verbose) {
    HT_INFOF("close fd=%d", fd);
  }
  m_open_file_map.remove(fd);
  cb->response_ok();
}


/**
 * Read
 */
void LocalBroker::read(ResponseCallbackRead *cb, uint32_t fd, uint32_t amount) {
  OpenFileDataLocalPtr fdata;
  ssize_t nread;
  uint64_t offset;
  StaticBuffer buf(new uint8_t [amount], amount);

  if (m_verbose) {
    HT_INFOF("read fd=%d amount=%d", fd, amount);
  }

  if (!m_open_file_map.get(fd, fdata)) {
    char errbuf[32];
    sprintf(errbuf, "%d", fd);
    cb->error(Error::DFSBROKER_BAD_FILE_HANDLE, errbuf);
    return;
  }

  if ((offset = (uint64_t)lseek(fdata->fd, 0, SEEK_CUR)) == (uint64_t)-1) {
    HT_ERRORF("lseek failed: fd=%d offset=0 SEEK_CUR - %s", fdata->fd,
              strerror(errno));
    report_error(cb);
    return;
  }

  if ((nread = FileUtils::read(fdata->fd, buf.base, amount)) == -1) {
    HT_ERRORF("read failed: fd=%d amount=%d - %s", fdata->fd, amount,
              strerror(errno));
    report_error(cb);
    return;
  }

  buf.size = nread;

  cb->response(offset, buf);
}


/**
 * Append
 */
void LocalBroker::append(ResponseCallbackAppend *cb, uint32_t fd,
                         uint32_t amount, const void *data, bool sync) {
  OpenFileDataLocalPtr fdata;
  ssize_t nwritten;
  uint64_t offset;

  if (m_verbose) {
    HT_INFOF("append fd=%d amount=%d", fd, amount);
  }

  if (!m_open_file_map.get(fd, fdata)) {
    char errbuf[32];
    sprintf(errbuf, "%d", fd);
    cb->error(Error::DFSBROKER_BAD_FILE_HANDLE, errbuf);
    return;
  }

  if ((offset = (uint64_t)lseek(fdata->fd, 0, SEEK_CUR)) == (uint64_t)-1) {
    HT_ERRORF("lseek failed: fd=%d offset=0 SEEK_CUR - %s", fdata->fd,
              strerror(errno));
    report_error(cb);
    return;
  }

  if ((nwritten = FileUtils::write(fdata->fd, data, amount)) == -1) {
    HT_ERRORF("write failed: fd=%d amount=%d - %s", fdata->fd, amount,
              strerror(errno));
    report_error(cb);
    return;
  }

  if (sync && fsync(fdata->fd) != 0) {
    HT_ERRORF("flush failed: fd=%d - %s", fdata->fd, strerror(errno));
    report_error(cb);
    return;
  }

  cb->response(offset, nwritten);

  return;
}


/**
 * Seek
 */
void LocalBroker::seek(ResponseCallback *cb, uint32_t fd, uint64_t offset) {
  OpenFileDataLocalPtr fdata;

  if (!m_open_file_map.get(fd, fdata)) {
    char errbuf[32];
    sprintf(errbuf, "%d", fd);
    cb->error(Error::DFSBROKER_BAD_FILE_HANDLE, errbuf);
    return;
  }

  if ((offset = (uint64_t)lseek(fdata->fd, offset, SEEK_SET)) == (uint64_t)-1) {
    HT_ERRORF("lseek failed: fd=%d offset=%lld - %s", fdata->fd, offset,
              strerror(errno));
    report_error(cb);
    return;
  }

  cb->response_ok();
}


/**
 * Remove
 */
void LocalBroker::remove(ResponseCallback *cb, const char *fname) {
  String abspath;

  if (m_verbose) {
    HT_INFOF("remove file='%s'", fname);
  }

  if (fname[0] == '/')
    abspath = m_rootdir + fname;
  else
    abspath = m_rootdir + "/" + fname;

  if (unlink(abspath.c_str()) == -1) {
    HT_ERRORF("unlink failed: file='%s' - %s", abspath.c_str(),
              strerror(errno));
    report_error(cb);
    return;
  }

  cb->response_ok();
}


/**
 * Length
 */
void LocalBroker::length(ResponseCallbackLength *cb, const char *fname) {
  String abspath;
  uint64_t length;

  if (m_verbose) {
    HT_INFOF("length file='%s'", fname);
  }

  if (fname[0] == '/')
    abspath = m_rootdir + fname;
  else
    abspath = m_rootdir + "/" + fname;

  if ((length = FileUtils::length(abspath)) == (uint64_t)-1) {
    HT_ERRORF("length (stat) failed: file='%s' - %s", abspath.c_str(),
              strerror(errno));
    report_error(cb);
    return;
  }

  cb->response(length);
}


/**
 * Pread
 */
void
LocalBroker::pread(ResponseCallbackRead *cb, uint32_t fd, uint64_t offset,
                   uint32_t amount) {
  OpenFileDataLocalPtr fdata;
  ssize_t nread;
  StaticBuffer buf(new uint8_t [amount], amount);

  if (m_verbose) {
    HT_INFOF("pread fd=%d offset=%lld amount=%d", fd, offset, amount);
  }

  if (!m_open_file_map.get(fd, fdata)) {
    char errbuf[32];
    sprintf(errbuf, "%d", fd);
    cb->error(Error::DFSBROKER_BAD_FILE_HANDLE, errbuf);
    return;
  }

  if ((nread = FileUtils::pread(fdata->fd, buf.base, amount, (off_t)offset))
      == -1) {
    HT_ERRORF("pread failed: fd=%d amount=%d offset=%lld - %s", fdata->fd,
              amount, offset, strerror(errno));
    report_error(cb);
    return;
  }

  buf.size = nread;

  cb->response(offset, buf);
}


/**
 * Mkdirs
 */
void LocalBroker::mkdirs(ResponseCallback *cb, const char *dname) {
  String absdir;

  if (m_verbose) {
    HT_INFOF("mkdirs dir='%s'", dname);
  }

  if (dname[0] == '/')
    absdir = m_rootdir + dname;
  else
    absdir = m_rootdir + "/" + dname;

  if (!FileUtils::mkdirs(absdir)) {
    HT_ERRORF("mkdirs failed: dname='%s' - %s", absdir.c_str(),
              strerror(errno));
    report_error(cb);
    return;
  }

  cb->response_ok();
}


/**
 * Rmdir
 */
void LocalBroker::rmdir(ResponseCallback *cb, const char *dname) {
  String absdir;
  String cmd_str;

  if (m_verbose) {
    HT_INFOF("rmdir dir='%s'", dname);
  }

  if (dname[0] == '/')
    absdir = m_rootdir + dname;
  else
    absdir = m_rootdir + "/" + dname;

  cmd_str = (String)"/bin/rm -rf " + absdir;
  if (system(cmd_str.c_str()) != 0) {
    HT_ERRORF("%s failed.", cmd_str.c_str());
    cb->error(Error::DFSBROKER_IO_ERROR, cmd_str);
    return;
  }

#if 0
  if (rmdir(absdir.c_str()) != 0) {
    HT_ERRORF("rmdir failed: dname='%s' - %s", absdir.c_str(), strerror(errno));
    report_error(cb);
    return;
  }
#endif

  cb->response_ok();
}

/**
 * Readdir
 */
void LocalBroker::readdir(ResponseCallbackReaddir *cb, const char *dname) {
  std::vector<String> listing;

  int Readdir(const char *pathname, std::vector<String> &result);


  String absdir;

  if (m_verbose) {
    HT_INFOF("Readdir dir='%s'", dname);
    std::cout << std::flush;
  }

  if (dname[0] == '/')
    absdir = m_rootdir + dname;
  else
    absdir = m_rootdir + "/" + dname;

  DIR *dirp = opendir(absdir.c_str());
  if (dirp == 0) {
    HT_ERRORF("opendir('%s') failed - %s", absdir.c_str(), strerror(errno));
    report_error(cb);
    return;
  }

  struct dirent dent;
  struct dirent *dp;

  if (readdir_r(dirp, &dent, &dp) != 0) {
    HT_ERRORF("readdir('%s') failed - %s", absdir.c_str(), strerror(errno));
    (void)closedir(dirp);
    report_error(cb);
    return;
  }

  while (dp != 0) {

    if (dp->d_name[0] != '.' && dp->d_name[0] != 0)
      listing.push_back((String)dp->d_name);

    if (readdir_r(dirp, &dent, &dp) != 0) {
      HT_ERRORF("readdir('%s') failed - %s", absdir.c_str(), strerror(errno));
      report_error(cb);
      return;
    }
  }
  (void)closedir(dirp);

  HT_INFOF("Sending back %d listings", listing.size());
  std::cout << std::flush;

  cb->response(listing);
}


/**
 * Flush
 */
void LocalBroker::flush(ResponseCallback *cb, uint32_t fd) {
  OpenFileDataLocalPtr fdata;

  if (m_verbose) {
    HT_INFOF("flush fd=%d", fd);
  }

  if (!m_open_file_map.get(fd, fdata)) {
    char errbuf[32];
    sprintf(errbuf, "%d", fd);
    cb->error(Error::DFSBROKER_BAD_FILE_HANDLE, errbuf);
    return;
  }

  if (fsync(fdata->fd) != 0) {
    HT_ERRORF("flush failed: fd=%d - %s", fdata->fd, strerror(errno));
    report_error(cb);
    return;
  }

  cb->response_ok();
}


/**
 */
void LocalBroker::status(ResponseCallback *cb) {
  cb->response_ok();
}


/**
 */
void LocalBroker::shutdown(ResponseCallback *cb) {
  m_open_file_map.remove_all();
  cb->response_ok();
  poll(0, 0, 2000);
}


void LocalBroker::exists(ResponseCallbackExists *cb, const char *fname) {
  String abspath;

  if (m_verbose) {
    HT_INFOF("exists file='%s'", fname);
  }

  if (fname[0] == '/')
    abspath = m_rootdir + fname;
  else
    abspath = m_rootdir + "/" + fname;

  cb->response(FileUtils::exists(abspath));
}


void
LocalBroker::rename(ResponseCallback *cb, const char *src, const char *dst) {
  String asrc =
    format("%s%s%s", m_rootdir.c_str(), *src == '/' ? "" : "/", src);
  String adst =
    format("%s%s%s", m_rootdir.c_str(), *dst == '/' ? "" : "/", dst);

  if (m_verbose)
    HT_INFOF("rename %s -> %s", asrc.c_str(), adst.c_str());

  if (std::rename(asrc.c_str(), adst.c_str()) != 0) {
    report_error(cb);
    return;
  }
  cb->response_ok();
}


/**
 * report_error
 */
void LocalBroker::report_error(ResponseCallback *cb) {
  char errbuf[128];
  errbuf[0] = 0;
  strerror_r(errno, errbuf, 128);
  if (errno == ENOTDIR || errno == ENAMETOOLONG || errno == ENOENT)
    cb->error(Error::DFSBROKER_BAD_FILENAME, errbuf);
  else if (errno == EACCES || errno == EPERM)
    cb->error(Error::DFSBROKER_PERMISSION_DENIED, errbuf);
  else if (errno == EBADF)
    cb->error(Error::DFSBROKER_BAD_FILE_HANDLE, errbuf);
  else if (errno == EINVAL)
    cb->error(Error::DFSBROKER_INVALID_ARGUMENT, errbuf);
  else
    cb->error(Error::DFSBROKER_IO_ERROR, errbuf);
}
