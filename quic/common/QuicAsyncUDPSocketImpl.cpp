/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifdef MVFST_USE_LIBEV

#include <quic/common/QuicAsyncUDPSocketImpl.h>

#include <cstring>
#include <sstream>
#include <stdexcept>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace quic {

QuicAsyncUDPSocketImpl::QuicAsyncUDPSocketImpl(QuicBackingEventBase* evb)
    : eventBase_(evb) {
  if (eventBase_) {
    // TODO: Enable this when this an event base?
    // DCHECK(eventBase_->isInEventBaseThread());
  }
}

QuicAsyncUDPSocketImpl::~QuicAsyncUDPSocketImpl() {
  if (fd_ != -1) {
    close();
  }
}

void QuicAsyncUDPSocketImpl::pauseRead() {
  LOG(INFO) << __func__;
  readCallback_ = nullptr;
}

void QuicAsyncUDPSocketImpl::resumeRead(ReadCallback* cb) {
  LOG(INFO) << __func__;
  readCallback_ = cb;
}

ssize_t QuicAsyncUDPSocketImpl::write(
    const folly::SocketAddress& address,
    const std::unique_ptr<folly::IOBuf>& buf) {
  if (fd_ == -1) {
    throw QuicAsyncUDPSocketException("socket is not initialized");
  }
  sockaddr_storage addrStorage;
  address.getAddress(&addrStorage);

  int msg_flags = 0;
  struct msghdr msg;

  if (!connected_) {
    msg.msg_name = reinterpret_cast<void*>(&addrStorage);
    msg.msg_namelen = sizeof(addrStorage);
  } else {
    if (connectedAddress_ != address) {
      throw QuicAsyncUDPSocketException(
          "wrong destination address for connected socket");
    }
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
  }

  iovec vec[16];
  size_t iovec_len = buf->fillIov(vec, sizeof(vec) / sizeof(vec[0])).numIovecs;
  if (UNLIKELY(iovec_len == 0)) {
    buf->coalesce();
    vec[0].iov_base = const_cast<uint8_t*>(buf->data());
    vec[0].iov_len = buf->length();
    iovec_len = 1;
  }

  msg.msg_iov = const_cast<struct iovec*>(vec);
  msg.msg_iovlen = iovec_len;
  msg.msg_control = nullptr;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;

  return ::sendmsg(fd_, &msg, msg_flags);
}

int QuicAsyncUDPSocketImpl::getGSO() {
  // TODO: Implement GSO
  return -1;
}

int QuicAsyncUDPSocketImpl::writem(
    folly::Range<folly::SocketAddress const*> /* addrs */,
    const std::unique_ptr<folly::IOBuf>* /* bufs */,
    size_t /* count */) {
  LOG(INFO) << __func__;
  return -1;
}

void QuicAsyncUDPSocketImpl::setAdditionalCmsgsFunc(
    folly::Function<folly::Optional<folly::SocketOptionMap>()>&&
    /* additionalCmsgsFunc */) {
  LOG(WARNING)
      << "Setting an additional cmsgs function is not implemented for QuicAsyncUDPSocketImpl";
}

bool QuicAsyncUDPSocketImpl::isBound() const {
  return bound_;
}

const folly::SocketAddress& QuicAsyncUDPSocketImpl::address() const {
  if (!bound_) {
    throw QuicAsyncUDPSocketException("socket is not bound");
  }
  return localAddress_;
}

void QuicAsyncUDPSocketImpl::attachEventBase(QuicBackingEventBase* /* evb */) {
  LOG(INFO) << __func__;
}

void QuicAsyncUDPSocketImpl::close() {
  LOG(INFO) << __func__;
}

void QuicAsyncUDPSocketImpl::detachEventBase() {
  LOG(INFO) << __func__;
}

void QuicAsyncUDPSocketImpl::setCmsgs(
    const folly::SocketOptionMap& /* cmsgs */) {
  throw std::runtime_error("setCmsgs is not implemented.");
}

void QuicAsyncUDPSocketImpl::appendCmsgs(
    const folly::SocketOptionMap& /* cmsgs */) {
  throw std::runtime_error("appendCmsgs is not implemented.");
}

void QuicAsyncUDPSocketImpl::init(sa_family_t family) {
  if (fd_ != -1) {
    // Socket already initialized.
    return;
  }

  if (family != AF_INET && family != AF_INET6) {
    throw QuicAsyncUDPSocketException("address family not supported");
  }

  NetworkFdType fd = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
  if (fd == -1) {
    throw QuicAsyncUDPSocketException("error creating socket", errno);
  }

  SCOPE_FAIL {
    ::close(fd);
  };

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    throw QuicAsyncUDPSocketException("error getting socket flags", errno);
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    throw QuicAsyncUDPSocketException(
        "error setting socket nonblocking flag", errno);
  }

  int sockOptVal = 1;
  if (reuseAddr_ &&
      ::setsockopt(
          fd, SOL_SOCKET, SO_REUSEADDR, &sockOptVal, sizeof(sockOptVal)) != 0) {
    throw QuicAsyncUDPSocketException(
        "error setting reuse address on socket", errno);
  }
  if (reusePort_ &&
      ::setsockopt(
          fd, SOL_SOCKET, SO_REUSEPORT, &sockOptVal, sizeof(sockOptVal)) != 0) {
    throw QuicAsyncUDPSocketException(
        "error setting reuse port on socket", errno);
  }

  fd_ = fd;
  ownership_ = FDOwnership::OWNS;
}

void QuicAsyncUDPSocketImpl::bind(const folly::SocketAddress& address) {
  // TODO: remove dependency on folly::SocketAdress since this pulls in
  // folly::portability and other headers which should be avoidable.
  if (fd_ == -1) {
    init(address.getFamily());
  }
  // bind to the address
  sockaddr_storage addrStorage;
  address.getAddress(&addrStorage);
  auto& saddr = reinterpret_cast<sockaddr&>(addrStorage);
  if (::bind(
          fd_,
          (struct sockaddr*)&saddr,
          saddr.sa_family == AF_INET6 ? sizeof(sockaddr_in6)
                                      : sizeof(sockaddr_in)) != 0) {
    throw QuicAsyncUDPSocketException(
        "error binding socket to " + address.describe(), errno);
  }

  bzero(&saddr, sizeof(saddr));
  socklen_t len = sizeof(saddr);
  if (::getsockname(fd_, &saddr, &len) != 0) {
    throw QuicAsyncUDPSocketException("error retrieving local address", errno);
  }

  localAddress_.setFromSockaddr(&saddr, len);
  bound_ = true;
}

void QuicAsyncUDPSocketImpl::connect(const folly::SocketAddress& address) {
  if (fd_ == -1) {
    init(address.getFamily());
  }

  sockaddr_storage addrStorage;
  address.getAddress(&addrStorage);
  auto saddr = reinterpret_cast<sockaddr&>(addrStorage);
  if (::connect(fd_, &saddr, sizeof(saddr)) != 0) {
    throw QuicAsyncUDPSocketException(
        "error connecting UDP socket to " + address.describe(), errno);
  }

  connected_ = true;
  connectedAddress_ = address;

  if (!localAddress_.isInitialized()) {
    bzero(&saddr, sizeof(saddr));
    socklen_t len = sizeof(saddr);
    if (::getsockname(fd_, &saddr, &len) != 0) {
      throw QuicAsyncUDPSocketException(
          "error retrieving local address", errno);
    }

    localAddress_.setFromSockaddr(&saddr, len);
  }
}

void QuicAsyncUDPSocketImpl::setDFAndTurnOffPMTU() {
  if (fd_ == -1) {
    throw QuicAsyncUDPSocketException("socket is not initialized");
  }
  int optname = 0;
  int optval = 0;
  int level = 0;
  switch (localAddress_.getFamily()) {
    case AF_INET:
      level = IPPROTO_IP;
      optname = IP_MTU_DISCOVER;
      optval = IP_PMTUDISC_PROBE;
      break;
    case AF_INET6:
      level = IPPROTO_IPV6;
      optname = IPV6_MTU_DISCOVER;
      optval = IPV6_PMTUDISC_PROBE;
      break;
  }
  if (optname && optval &&
      ::setsockopt(fd_, level, optname, &optval, sizeof(optval)) != 0) {
    throw QuicAsyncUDPSocketException("Failed to disable pmtud", errno);
  }
}

void QuicAsyncUDPSocketImpl::setErrMessageCallback(
    ErrMessageCallback* errMessageCallback) {
  errMessageCallback_ = errMessageCallback;
}

int QuicAsyncUDPSocketImpl::getGRO() {
  LOG(INFO) << __func__;
  return -1;
}

ssize_t QuicAsyncUDPSocketImpl::recvmsg(
    struct msghdr* /* msg */,
    int /* flags */) {
  LOG(INFO) << __func__;
  return -1;
}

int QuicAsyncUDPSocketImpl::recvmmsg(
    struct mmsghdr* msgvec,
    unsigned int vlen,
    unsigned int flags,
    struct timespec* timeout) {
  LOG(INFO) << __func__;
  return ::recvmmsg(fd_, msgvec, vlen, (int)flags, timeout);
}

bool QuicAsyncUDPSocketImpl::setGRO(bool /* bVal */) {
  LOG(INFO) << __func__;
  return useGro_;
}

void QuicAsyncUDPSocketImpl::applyOptions(
    const folly::SocketOptionMap& options,
    folly::SocketOptionKey::ApplyPos pos) {
  for (const auto& opt : options) {
    if (opt.first.applyPos_ == pos) {
      if (::setsockopt(
              fd_,
              opt.first.level,
              opt.first.optname,
              &opt.second,
              sizeof(opt.second)) != 0) {
        throw QuicAsyncUDPSocketException(
            "failed to apply socket options", errno);
      }
    }
  }
}

QuicBackingEventBase* QuicAsyncUDPSocketImpl::getEventBase() const {
  return eventBase_;
}

void QuicAsyncUDPSocketImpl::setFD(NetworkFdType fd, FDOwnership ownership) {
  LOG(INFO) << __func__;
  fd_ = fd;
  ownership_ = ownership;
}

// STATIC
void QuicAsyncUDPSocketImpl::fromMsg(
    ReadCallback::OnDataAvailableParams& /* params */,
    struct msghdr& /* msg */) {
  LOG(INFO) << __func__;
}

// STATIC
std::string QuicAsyncUDPSocketException::getMessage(
    const std::string& message,
    int errnoCopy) {
  std::stringstream msgStream;
  msgStream << message << ". errno=" << errnoCopy << " ("
            << std::strerror(errnoCopy) << ")";
  return msgStream.str();
}

} // namespace quic

#endif