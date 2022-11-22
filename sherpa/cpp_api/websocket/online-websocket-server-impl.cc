/**
 * Copyright      2022  Xiaomi Corporation (authors: Fangjun Kuang)
 *
 * See LICENSE for clarification regarding multiple authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sherpa/cpp_api/websocket/online-websocket-server-impl.h"

#include "sherpa/csrc/file-utils.h"
#include "sherpa/csrc/log.h"

namespace sherpa {

void OnlineWebsocketDecoderConfig::Register(ParseOptions *po) {
  recognizer_config.Register(po);
}

void OnlineWebsocketDecoderConfig::Validate() const {
  recognizer_config.Validate();
}

void OnlineWebsocketServerConfig::Register(sherpa::ParseOptions *po) {
  decoder_config.Register(po);
  po->Register("doc-root", &doc_root,
               "Path to the directory where "
               "files like index.html for the HTTP server locate.");

  po->Register("log-file", &log_file,
               "Path to the log file. Logs are "
               "appended to this file");
}

void OnlineWebsocketServerConfig::Validate() const {
  decoder_config.Validate();

  if (doc_root.empty()) {
    SHERPA_LOG(FATAL) << "Please provide --doc-root, e.g., sherpa/bin/web";
  }

  if (!FileExists(doc_root + "/index.html")) {
    SHERPA_LOG(FATAL) << "\n--doc-root=" << doc_root << "\n"
                      << doc_root << "/index.html does not exist!\n"
                      << "Make sure that you use sherpa/bin/web/ as --doc-root";
  }
}

OnlineWebsocketDecoder::OnlineWebsocketDecoder(OnlineWebsocketServer *server)
    : server_(server), config_(server->GetConfig().decoder_config) {
  recognizer_ = std::make_unique<OnlineRecognizer>(config_.recognizer_config);
}

void OnlineWebsocketDecoder::Push(connection_hdl hdl,
                                  std::shared_ptr<OnlineStream> s) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_.count(s.get()) > 0) {
    return;
  }

  streams_.push_back({hdl, s});
  active_.insert(s.get());
}

void OnlineWebsocketDecoder::Decode() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (streams_.empty()) {
    return;
  }

  auto pair = streams_.front();
  streams_.pop_front();
  lock.unlock();

  auto hdl = pair.first;
  auto s = pair.second;

  recognizer_->DecodeStream(s.get());

  auto result = recognizer_->GetResult(s.get());
  asio::post(server_->GetConnectionContext(),
             [this, hdl, json = result.AsJsonString()]() {
               server_->Send(hdl, json);
             });

  if (server_->Contains(hdl) && recognizer_->IsReady(s.get())) {
    // If the connection is still alive and the stream is ready
    lock.lock();
    streams_.push_back({hdl, s});
    lock.unlock();
    asio::post(server_->GetWorkContext(), [this]() { this->Decode(); });
  } else {
    lock.lock();
    active_.erase(s.get());
    lock.unlock();

    if (s->IsLastFrame(s->NumFramesReady() - 1)) {
      asio::post(server_->GetConnectionContext(),
                 [this, hdl]() { server_->Send(hdl, "Done"); });
    }
  }
}

OnlineWebsocketServer::OnlineWebsocketServer(
    asio::io_context &io_conn, asio::io_context &io_work,
    const OnlineWebsocketServerConfig &config)
    : config_(config),
      io_conn_(io_conn),
      io_work_(io_work),
      http_server_(config.doc_root),
      log_(config.log_file, std::ios::app),
      tee_(std::cout, log_),
      decoder_(this) {
  SetupLog();

  server_.init_asio(&io_conn_);

  server_.set_open_handler([this](connection_hdl hdl) { OnOpen(hdl); });

  server_.set_close_handler([this](connection_hdl hdl) { OnClose(hdl); });

  server_.set_http_handler([this](connection_hdl hdl) { OnHttp(hdl); });

  server_.set_message_handler(
      [this](connection_hdl hdl, server::message_ptr msg) {
        OnMessage(hdl, msg);
      });
}

void OnlineWebsocketServer::Run(uint16_t port) {
  server_.set_reuse_addr(true);
  server_.listen(asio::ip::tcp::v4(), port);
  server_.start_accept();
}

void OnlineWebsocketServer::SetupLog() {
  server_.clear_access_channels(websocketpp::log::alevel::all);
  server_.set_access_channels(websocketpp::log::alevel::connect);
  server_.set_access_channels(websocketpp::log::alevel::disconnect);

  // So that it also prints to std::cout and std::cerr
  server_.get_alog().set_ostream(&tee_);
  server_.get_elog().set_ostream(&tee_);
}

void OnlineWebsocketServer::Send(connection_hdl hdl, const std::string &text) {
  websocketpp::lib::error_code ec;
  if (!Contains(hdl)) {
    return;
  }

  server_.send(hdl, text, websocketpp::frame::opcode::text, ec);
  if (ec) {
    server_.get_alog().write(websocketpp::log::alevel::app, ec.message());
  }
}

void OnlineWebsocketServer::OnOpen(connection_hdl hdl) {
  std::lock_guard<std::mutex> lock(mutex_);
  SHERPA_LOG(INFO) << "New connection: "
                   << server_.get_con_from_hdl(hdl)->get_remote_endpoint();

  connections_.emplace(hdl, decoder_.GetRecognizer()->CreateStream());

  SHERPA_LOG(INFO) << "Number of active connections: " << connections_.size()
                   << "\n";
}
void OnlineWebsocketServer::OnClose(connection_hdl hdl) {
  std::lock_guard<std::mutex> lock(mutex_);
  connections_.erase(hdl);

  SHERPA_LOG(INFO) << "Number of active connections: " << connections_.size()
                   << "\n";
}

bool OnlineWebsocketServer::Contains(connection_hdl hdl) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return connections_.count(hdl);
}

void OnlineWebsocketServer::OnHttp(connection_hdl hdl) {
  auto con = server_.get_con_from_hdl(hdl);

  std::string filename = con->get_resource();
  if (filename == "/") filename = "/index.html";

  std::string content;
  bool found = false;

  if (filename != "/upload.html" && filename != "/offline_record.html") {
    found = http_server_.ProcessRequest(filename, &content);
  } else {
    content = R"(
<!doctype html><html><head>
<title>Speech recognition with next-gen Kaldi</title><body>
<h2>Only /streaming_record.html is available for the online server.<h2>
<br/>
<br/>
Go back to <a href="/streaming_record.html">/streaming_record.html</a>
</body></head></html>
    )";
  }

  if (found) {
    con->set_status(websocketpp::http::status_code::ok);
  } else {
    con->set_status(websocketpp::http::status_code::not_found);
  }

  con->set_body(std::move(content));
}

void OnlineWebsocketServer::OnMessage(connection_hdl hdl,
                                      server::message_ptr msg) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto stream = connections_.find(hdl)->second;
  lock.unlock();
  const std::string &payload = msg->get_payload();

  auto recognizer = decoder_.GetRecognizer();
  float sample_rate =
      decoder_.GetConfig()
          .recognizer_config.feat_config.fbank_opts.frame_opts.samp_freq;

  switch (msg->get_opcode()) {
    case websocketpp::frame::opcode::text:
      if (payload == "Done") {
        torch::Tensor tail_padding =
            torch::zeros({static_cast<int64_t>(0.3 * sample_rate)})
                .to(torch::kFloat);

        stream->AcceptWaveform(sample_rate, tail_padding);
        stream->InputFinished();
        if (recognizer->IsReady(stream.get())) {
          decoder_.Push(hdl, stream);

          asio::post(io_work_, [this]() { decoder_.Decode(); });
        }
      }
      break;
    case websocketpp::frame::opcode::binary: {
      auto p = reinterpret_cast<const float *>(payload.data());
      int32_t num_samples = payload.size() / sizeof(float);
      torch::Tensor samples = torch::from_blob(const_cast<float *>(p),
                                               {num_samples}, torch::kFloat);
      // Caution(fangjun): We have to make a copy here since the tensor
      // is referenced inside the fbank computer.
      // Otherwise, it will cause segfault for the next invocation
      // of AcceptWaveform since payload is freed after this function returns
      samples = samples.clone();
      stream->AcceptWaveform(sample_rate, samples);

      if (recognizer->IsReady(stream.get())) {
        decoder_.Push(hdl, stream);
        asio::post(io_work_, [this]() { decoder_.Decode(); });
      }

      break;
    }
    default:
      break;
  }
}

void OnlineWebsocketServer::Close(connection_hdl hdl,
                                  websocketpp::close::status::value code,
                                  const std::string &reason) {
  auto con = server_.get_con_from_hdl(hdl);

  std::ostringstream os;
  os << "Closing " << con->get_remote_endpoint() << " with reason: " << reason
     << "\n";

  websocketpp::lib::error_code ec;
  server_.close(hdl, code, reason, ec);
  if (ec) {
    os << "Failed to close" << con->get_remote_endpoint() << ". "
       << ec.message() << "\n";
  }
  server_.get_alog().write(websocketpp::log::alevel::app, os.str());
}

}  // namespace sherpa