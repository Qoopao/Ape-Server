#ifndef APE_GRPC_ASYNC_UTIL_H
#define APE_GRPC_ASYNC_UTIL_H

#include <grpcpp/grpcpp.h>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/async_result.hpp>

namespace ape {
namespace grpc_util {

/// Bridge a gRPC async unary call into a boost::asio::awaitable<grpc::Status>.
/// The InitFunc is invoked with a completion handler that satisfies
/// void(grpc::Status).  When the gRPC callback fires, the coroutine resumes
/// on the executor that the caller is bound to.
///
/// Usage inside a coroutine:
///
///   auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
///       stub_->async()->SomeMethod(&ctx, &req, &resp,
///           std::forward<decltype(handler)>(handler));
///   });
///
template <typename InitFunc>
auto GrpcAwait(InitFunc&& init) -> boost::asio::awaitable<grpc::Status> {
    return boost::asio::async_initiate<
        decltype(boost::asio::use_awaitable),
        void(grpc::Status)>(
        [init = std::forward<InitFunc>(init)](auto&& handler) mutable {
            // handler 是 move-only 的 awaitable_handler，不能直接传给
            // gRPC 的 std::function 回调。通过 shared_ptr 间接持有，
            // 再传递一个可拷贝的 lambda 给 gRPC。
            auto handler_ptr = std::make_shared<std::decay_t<decltype(handler)>>(
                std::move(handler));
            init([handler_ptr](grpc::Status status) {
                (*handler_ptr)(std::move(status));
            });
        },
        boost::asio::use_awaitable);
}

}  // namespace grpc_util
}  // namespace ape

#endif