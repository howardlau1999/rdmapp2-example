#include <cassert>
#include <coro/coro.hpp>
#include <iostream>
#include <thread>

#include <rdmapp/rdmapp.h>

auto device = std::make_shared<rdmapp::device>(0, 1);
auto pd = std::make_shared<rdmapp::pd>(device.get());
auto cq = std::make_shared<rdmapp::cq>(device.get());
auto qp = new rdmapp::qp(pd.get(), cq.get(), cq.get(), nullptr);

std::atomic<bool> stopped;

void cq_poller() {
  struct ibv_wc wc;
  while (!stopped) {
    auto event = cq->poll(wc);
    if (event) {
      rdmapp::process_wc(wc);
    }
  }
}

coro::task<void> handle_qp(rdmapp::qp *qp) {
  /* Send/Recv */
  char buffer[2048] = "hello";
  auto local_buffer_mr =
      new rdmapp::local_mr(qp->pd_ptr()->reg_mr(buffer, sizeof(buffer)));
  co_await qp->send(buffer, 6, local_buffer_mr->lkey());
  std::cout << "Sent to client: " << buffer << std::endl;
  co_await qp->recv(buffer, sizeof(buffer), local_buffer_mr->lkey());
  std::cout << "Received from client: " << buffer << std::endl;

  /* Read/Write */
  char rw_buffer[2048];
  std::copy_n("hello", sizeof(rw_buffer), rw_buffer);
  auto rw_mr =
      new rdmapp::local_mr(qp->pd_ptr()->reg_mr(rw_buffer, sizeof(rw_buffer)));
  auto local_mr_serialized = rw_mr->serialize();
  std::copy(local_mr_serialized.begin(), local_mr_serialized.end(), buffer);
  co_await qp->send(buffer, local_mr_serialized.size(),
                    local_buffer_mr->lkey());
  std::cout << "Sent mr addr=" << rw_mr->addr() << " length=" << rw_mr->length()
            << " rkey=" << rw_mr->rkey() << " to client" << std::endl;
  auto [_, imm] = co_await qp->recv(rw_mr);
  assert(imm.has_value());
  std::cout << "Written by client (imm=" << imm.value() << "): " << buffer
            << std::endl;

  /* Atomic */
  uint64_t counter = 42;
  auto counter_mr =
      new rdmapp::local_mr(qp->pd_ptr()->reg_mr(&counter, sizeof(counter)));
  auto counter_mr_serialized = counter_mr->serialize();
  std::copy(counter_mr_serialized.begin(), counter_mr_serialized.end(), buffer);
  co_await qp->send(buffer, counter_mr_serialized.size(),
                    local_buffer_mr->lkey());
  std::cout << "Sent mr addr=" << counter_mr->addr()
            << " length=" << counter_mr->length()
            << " rkey=" << counter_mr->rkey() << " to client" << std::endl;
  imm = (co_await qp->recv(local_buffer_mr)).second;
  assert(imm.has_value());
  std::cout << "Fetched and added by client: " << counter << std::endl;
  imm = (co_await qp->recv(local_buffer_mr)).second;
  assert(imm.has_value());
  std::cout << "Compared and swapped by client: " << counter << std::endl;
  stopped = true;
  co_return;
}

coro::task<void> client_qp(rdmapp::qp *qp) {
  char buffer[2048];
  auto local_buffer_mr =
      new rdmapp::local_mr(qp->pd_ptr()->reg_mr(buffer, sizeof(buffer)));
  /* Send/Recv */
  auto [n, _] =
      co_await qp->recv(buffer, sizeof(buffer), local_buffer_mr->lkey());
  std::cout << "Received " << n << " bytes from server: " << buffer
            << std::endl;
  std::copy_n("world", sizeof(buffer), buffer);
  co_await qp->send(buffer, 6, local_buffer_mr->lkey());
  std::cout << "Sent to server: " << buffer << std::endl;

  /* Read/Write */
  co_await qp->recv(buffer, sizeof(buffer), local_buffer_mr->lkey());
  auto remote_mr = rdmapp::remote_mr::deserialize(buffer);
  std::cout << "Received mr addr=" << remote_mr.addr()
            << " length=" << remote_mr.length() << " rkey=" << remote_mr.rkey()
            << " from server" << std::endl;
  n = co_await qp->read(remote_mr.addr(), remote_mr.length(), remote_mr.rkey(),
                        buffer, 6, local_buffer_mr->lkey());
  std::cout << "Read " << n << " bytes from server: " << buffer << std::endl;
  std::copy_n("world", sizeof(buffer), buffer);
  co_await qp->write_with_imm(remote_mr.addr(), remote_mr.length(),
                              remote_mr.rkey(), buffer, sizeof(buffer),
                              local_buffer_mr->lkey(), 1);

  /* Atomic Fetch-and-Add (FA)/Compare-and-Swap (CS) */
  co_await qp->recv(buffer, sizeof(buffer), local_buffer_mr->lkey());
  auto counter_mr = rdmapp::remote_mr::deserialize(buffer);
  std::cout << "Received mr addr=" << counter_mr.addr()
            << " length=" << counter_mr.length()
            << " rkey=" << counter_mr.rkey() << " from server" << std::endl;
  uint64_t counter = 0;
  auto local_counter_mr =
      new rdmapp::local_mr(qp->pd_ptr()->reg_mr(&counter, sizeof(counter)));
  co_await qp->fetch_and_add(counter_mr.addr(), counter_mr.length(),
                             counter_mr.rkey(), &counter, sizeof(counter),
                             local_counter_mr->lkey(), 1);
  std::cout << "Fetched and added from server: " << counter << std::endl;
  co_await qp->write_with_imm(remote_mr.addr(), remote_mr.length(),
                              remote_mr.rkey(), buffer, sizeof(buffer),
                              local_buffer_mr->lkey(), 1);
  co_await qp->compare_and_swap(counter_mr.addr(), counter_mr.length(),
                                counter_mr.rkey(), &counter, sizeof(counter),
                                local_counter_mr->lkey(), 43, 4422);
  std::cout << "Compared and swapped from server: " << counter << std::endl;
  std::copy_n("world", sizeof(buffer), buffer);
  co_await qp->write_with_imm(remote_mr.addr(), remote_mr.length(),
                              remote_mr.rkey(), buffer, sizeof(buffer),
                              local_buffer_mr->lkey(), 1);
  stopped = true;
  co_return;
}

coro::task<void> send_all(coro::net::tcp_client &client,
                          std::span<const char> remaining) {
  do {
    auto pstatus = co_await client.poll(coro::poll_op::write);
    if (pstatus != coro::poll_status::event) {
      co_return; // Handle error.
    }

    // Optimistically send() prior to polling.
    auto [send_status, r] = client.send(remaining);
    if (send_status != coro::net::send_status::ok) {
      co_return; // Handle error, see net::send_status for detailed error
                 // states.
    }

    if (r.empty()) {
      break; // The entire message has been sent.
    }

    // Re-assign remaining bytes for the next loop iteration and poll for the
    // socket to be able to be written to again.
    remaining = r;
  } while (true);
}

coro::task<void> send_qp(coro::net::tcp_client &client, rdmapp::qp *qp) {
  auto const &local_qp_data = qp->serialize();
  std::cout << "Serialized qp data size: " << local_qp_data.size() << std::endl;
  co_await send_all(client, std::string_view(reinterpret_cast<const char *>(
                                                 local_qp_data.data()),
                                             local_qp_data.size()));
  co_return;
}

coro::task<rdmapp::deserialized_qp> recv_qp(coro::net::tcp_client &client) {
  size_t header_read = 0;
  uint8_t header[rdmapp::deserialized_qp::qp_header::kSerializedSize];
  while (header_read < rdmapp::deserialized_qp::qp_header::kSerializedSize) {
    co_await client.poll(coro::poll_op::read);
    auto [recv_status, recv_bytes] = client.recv(std::span(
        reinterpret_cast<char *>(header) + header_read,
        rdmapp::deserialized_qp::qp_header::kSerializedSize - header_read));
    header_read += recv_bytes.size();
  }
  auto remote_qp = rdmapp::deserialized_qp::deserialize(header);
  std::printf("received header lid=%u qpn=%u psn=%u user_data_size=%u\n",
              remote_qp.header.lid, remote_qp.header.qp_num,
              remote_qp.header.sq_psn, remote_qp.header.user_data_size);
  remote_qp.user_data.resize(remote_qp.header.user_data_size);

  if (remote_qp.header.user_data_size > 0) {
    size_t user_data_read = 0;
    remote_qp.user_data.resize(remote_qp.header.user_data_size, 0);
    while (user_data_read < remote_qp.header.user_data_size) {
      co_await client.poll(coro::poll_op::read);
      auto [recv_status, recv_bytes] = client.recv(std::span(
          reinterpret_cast<char *>(remote_qp.user_data.data()) + user_data_read,
          remote_qp.header.user_data_size - user_data_read));
      user_data_read += recv_bytes.size();
    }
  }
  co_return remote_qp;
}

auto main(int argc, char *argv[]) -> int {

  auto scheduler =
      std::make_shared<coro::io_scheduler>(coro::io_scheduler::options{
          // The scheduler will spawn a dedicated event processing thread.  This
          // is the default, but
          // it is possible to use 'manual' and call 'process_events()' to drive
          // the scheduler yourself.
          .thread_strategy = coro::io_scheduler::thread_strategy_t::spawn,
          // If the scheduler is in spawn mode this functor is called upon
          // starting the dedicated
          // event processor thread.
          .on_io_thread_start_functor =
              [] { std::cout << "io_scheduler::process event thread start\n"; },
          // If the scheduler is in spawn mode this functor is called upon
          // stopping the dedicated
          // event process thread.
          .on_io_thread_stop_functor =
              [] { std::cout << "io_scheduler::process event thread stop\n"; },
          // The io scheduler can use a coro::thread_pool to process the events
          // or tasks it is given.
          // You can use an execution strategy of `process_tasks_inline` to have
          // the event loop thread
          // directly process the tasks, this might be desirable for small tasks
          // vs a thread pool for large tasks.
          .pool =
              coro::thread_pool::options{
                  .thread_count = 1,
                  .on_thread_start_functor =
                      [](size_t i) {
                        std::cout << "io_scheduler::thread_pool worker " << i
                                  << " starting\n";
                      },
                  .on_thread_stop_functor =
                      [](size_t i) {
                        std::cout << "io_scheduler::thread_pool worker " << i
                                  << " stopping\n";
                      },
              },
          .execution_strategy = coro::io_scheduler::execution_strategy_t::
              process_tasks_on_thread_pool});
  auto make_server_task = [&](std::string const &port) -> coro::task<void> {
    // Start by creating a tcp server, we'll do this before putting it into the
    // scheduler so it is immediately available for the client to connect since
    // this will create a socket, bind the socket and start listening on that
    // socket.  See tcp_server for more details on how to specify the local
    // address and port to bind to as well as enabling SSL/TLS.
    coro::net::tcp_server::options options;
    options.port = std::stoi(port);
    coro::net::tcp_server server{scheduler};

    // Now scheduler this task onto the scheduler.
    co_await scheduler->schedule();

    // Wait for an incoming connection and accept it.
    auto poll_status = co_await server.poll();
    if (poll_status != coro::poll_status::event) {
      co_return; // Handle error, see poll_status for detailed error states.
    }

    // Accept the incoming client connection.
    auto client = server.accept();

    // Verify the incoming connection was accepted correctly.
    if (!client.socket().is_valid()) {
      co_return; // Handle error.
    }

    // Now wait for the client message, this message is small enough it should
    // always arrive with a single recv() call.
    poll_status = co_await client.poll(coro::poll_op::read);
    if (poll_status != coro::poll_status::event) {
      co_return; // Handle error.
    }

    auto remote_qp = co_await recv_qp(client);
    qp->rtr(remote_qp.header.lid, remote_qp.header.qp_num,
            remote_qp.header.sq_psn);
    qp->rts();

    co_await send_qp(client, qp);

    co_await handle_qp(qp);

    co_return;
  };

  auto make_client_task = [&](std::string const &ip,
                              std::string const &port) -> coro::task<void> {
    // Immediately schedule onto the scheduler.
    co_await scheduler->schedule();

    // Create the tcp_client with the default settings, see tcp_client for how
    // to set the ip address, port, and optionally enabling SSL/TLS.
    coro::net::tcp_client::options options;
    options.address = coro::net::ip_address::from_string(ip);
    options.port = std::stoi(port);
    coro::net::tcp_client client{scheduler};

    // Connect to the server.
    co_await client.connect();

    // Make sure the client socket can be written to.
    co_await client.poll(coro::poll_op::write);

    // Send the request data.
    co_await send_qp(client, qp);

    // Wait for the response and receive it.
    auto remote_qp = co_await recv_qp(client);
    qp->rtr(remote_qp.header.lid, remote_qp.header.qp_num,
            remote_qp.header.sq_psn);
    qp->rts();

    co_await client_qp(qp);

    co_return;
  };
  std::jthread poller = std::jthread(cq_poller);
  if (argc == 2) {
    coro::sync_wait(make_server_task(argv[1]));
  } else if (argc == 3) {
    coro::sync_wait(make_client_task(argv[1], argv[2]));
  }

  return 0;
}