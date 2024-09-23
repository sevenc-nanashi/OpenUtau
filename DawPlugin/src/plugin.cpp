#include "plugin.hpp"
#include "DistrhoDetails.hpp"
#include "DistrhoPlugin.hpp"
#include "DistrhoPluginInfo.h"
#include "asio.hpp"
#include "choc/containers/choc_Value.h"
#include "choc/memory/choc_Base64.h"
#include "choc/text/choc_JSON.h"
#include "common.hpp"
#include "extra/String.hpp"
#include "gzip/compress.hpp"
#include "uuid/v4/uuid.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

// std::jthread *ioThread = nullptr;
namespace Network {
// std::jthread *ioThread = nullptr;
std::shared_ptr<std::jthread> ioThread;
std::shared_ptr<asio::io_context> ioContext;
std::shared_ptr<asio::io_context> getIoContext() {
  if (ioThread == nullptr) {
    if (ioContext == nullptr) {
      ioContext = std::make_shared<asio::io_context>();
    }
    ioThread = std::make_shared<std::jthread>([](std::stop_token st) {
      while (!st.stop_requested()) {
        ioContext->run();
      }
    });
    std::atexit([]() {
      ioContext->stop();
      ioThread->request_stop();
      ioThread->join();
    });
  }

  return ioContext;
}
} // namespace Network

// note: OpenUtau returns 44100Hz, 2ch, 32bit float audio

choc::value::Value Part::serialize() const {
  return choc::value::createObject("", "trackNo", trackNo, "startMs", startMs,
                                   "endMs", endMs, "audioHash", (int64_t)hash);
}
Part Part::deserialize(const choc::value::ValueView &value) {
  Part part;
  part.trackNo = value["trackNo"].get<int>();
  part.startMs = value["startMs"].get<double>();
  part.endMs = value["endMs"].get<double>();
  part.hash = value["audioHash"].get<uint32_t>();
  return part;
}

// -----------------------------------------------------------------------------------------------------------
OpenUtauPlugin::OpenUtauPlugin()
    : Plugin(0, 0, 5)

{

  if (this->isDummyInstance()) {
    return;
  }

  std::string uuid = uuid::v4::UUID::New().String();
  setState("uuid", uuid.c_str());

  setState("name", uuid.c_str());

  this->connected = false;

  initializeNetwork();
}
OpenUtauPlugin::~OpenUtauPlugin() {
  if (this->isDummyInstance()) {
    return;
  }

  if (std::filesystem::exists(this->socketPath)) {
    std::filesystem::remove(this->socketPath);
  }

  if (this->acceptor != nullptr) {
    this->acceptor->close();
  }
  if (this->acceptorThread != nullptr) {
    this->acceptorThread->request_stop();
  }
  while (this->connected) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

/* --------------------------------------------------------------------------------------------------------
 * Information */

/**
                               Get the plugin label.
                               This label is a short restricted name consisting
   of only _, a-z, A-Z and 0-9 characters.
 */
const char *OpenUtauPlugin::getLabel() const { return "OpenUtau"; }

/**
   Get an extensive comment/description about the plugin.
 */
const char *OpenUtauPlugin::getDescription() const {
  return "Plugin to show how to get some basic information sent to the UI.";
}

/**
   Get the plugin author/maker.
 */
const char *OpenUtauPlugin::getMaker() const { return "stakira"; }

/**
   Get the plugin homepage.
 */
const char *OpenUtauPlugin::getHomePage() const {
  return "https://github.com/stakira/OpenUtau/";
}

void OpenUtauPlugin::initState(uint32_t index, State &state) {
  switch (index) {
  case 0:
    state.key = "name";
    state.label = "Plugin Name";
    state.defaultValue = "";
    break;
  case 1:
    state.key = "ustx";
    state.label = "USTx";
    state.hints = kStateIsBase64Blob;
    break;
  case 2:
    state.key = "audios";
    state.label = "Audios";
    state.hints = kStateIsBase64Blob;
    break;
  case 3:
    state.key = "parts";
    state.label = "Parts";
    break;
  case 4:
    state.key = "tracks";
    state.label = "Tracks";
    state.hints = kStateIsBase64Blob;
    break;
  case 5:
    state.key = "mapping";
    state.label = "Output Mapping";
    break;
  }
}

String OpenUtauPlugin::getState(const char *rawKey) const {
  // DPF cannot handle binary data, so we need to encode it to base64
  std::string key(rawKey);

  if (key == "name") {
    return String(name.c_str());
  } else if (key == "uuid") {
    return String(uuid.c_str());
  } else if (key == "ustx") {
    std::string encoded = choc::base64::encodeToString(ustx);
    return String(encoded.c_str());
  } else if (key == "audios") {
    choc::value::Value value = choc::value::createObject("");
    for (const auto &[audioHash, audio] : audioBuffers) {
      std::string compressed =
          gzip::compress((char *)audio.data(), audio.size() * sizeof(float));
      std::string encoded = choc::base64::encodeToString(compressed);
      value.setMember(std::to_string(audioHash), encoded);
    }

    return String(choc::json::toString(value).c_str());
  } else if (key == "parts") {
    choc::value::Value value = choc::value::createEmptyArray();
    for (const auto &[trackNo, parts] : parts) {
      for (const auto &part : parts) {
        value.addArrayElement(part.serialize());
      }
    }
    return String(choc::json::toString(value).c_str());
  } else if (key == "tracks") {
    return String(Structures::serializeTracks(tracks).c_str());
  } else if (key == "mapping") {
    return String(Structures::serializeOutputMap(outputMap).c_str());
  }
  return String();
}

void OpenUtauPlugin::setState(const char *rawKey, const char *value) {
  std::string key(rawKey);
  if (key == "name") {
    this->name = value;
    this->updatePluginServerFile();
  } else if (key == "uuid") {
    this->uuid = value;
  } else if (key == "ustx") {
    this->ustx = Utils::unBase64ToString(value);
  } else if (key == "audios") {
    choc::value::Value audioValue = choc::json::parse(value);
    std::map<AudioHash, std::vector<float>> audioBuffers;
    choc::value::ValueView(audioValue)
        .visitObjectMembers(
            [&](std::string_view key, const choc::value::ValueView &value) {
              auto hash = std::stoul(std::string(key));
              std::string encoded = value.get<std::string>();
              auto decoded = Utils::unBase64ToVector(encoded);
              auto decompressed =
                  Utils::gunzip((char *)decoded.data(), decoded.size());
              std::vector<float> audio((float *)decompressed.data(),
                                       (float *)decompressed.data() +
                                           decompressed.size() / sizeof(float));
              audioBuffers[hash] = audio;
            });

    {
      auto _lock = std::lock_guard(this->audioBuffersMutex);
      this->audioBuffers = audioBuffers;
    }
    this->requestResampleMixes(this->currentSampleRate);
  } else if (key == "parts") {
    choc::value::Value partsValue = choc::json::parse(value);
    std::map<int, std::vector<Part>> parts;
    for (const auto &partValue : partsValue) {
      Part part = Part::deserialize(partValue);
      if (parts.find(part.trackNo) == parts.end()) {
        parts[part.trackNo] = std::vector<Part>();
      }
      parts[part.trackNo].push_back(part);
    }
    {
      auto _lock = std::lock_guard(this->partsMutex);
      this->parts = parts;
    }
    this->requestResampleMixes(this->currentSampleRate);
  } else if (key == "tracks") {
    this->tracks = Structures::deserializeTracks(value);
    syncMapping();
  } else if (key == "mapping") {
    this->outputMap = Structures::deserializeOutputMap(value);
  }
}

/**
   Get the plugin license name (a single line of text).
   For commercial plugins this should return some short copyright information.
 */
const char *OpenUtauPlugin::getLicense() const { return "ISC"; }

/**
   Get the plugin version, in hexadecimal.
 */
uint32_t OpenUtauPlugin::getVersion() const {
  return d_version(Constants::majorVersion, Constants::minorVersion,
                   Constants::patchVersion);
}

/* --------------------------------------------------------------------------------------------------------
 * Init */

/**
                               Initialize the audio port @a index.@n
                               This function will be called once, shortly after
   the plugin is created.
 */
void OpenUtauPlugin::initAudioPort(bool input, uint32_t index,
                                   AudioPort &port) {
  port.groupId = index / 2;
  port.hints = kPortGroupStereo;
  auto name = std::format("Channel {}", index / 2 + 1);
  port.name = String(name.c_str());
}

/* --------------------------------------------------------------------------------------------------------
 * Audio/MIDI Processing */

void OpenUtauPlugin::run(const float **inputs, float **outputs, uint32_t frames,
                         const MidiEvent *midiEvents, uint32_t midiEventCount) {

  auto timePosition = this->getTimePosition();

  for (uint32_t i = 0; i < DISTRHO_PLUGIN_NUM_OUTPUTS; ++i) {
    for (uint32_t j = 0; j < frames; ++j) {
      outputs[i][j] = 0;
    }
  }

  auto sampleRate = getSampleRate();
  auto lock = std::shared_lock(this->mixMutex, std::defer_lock);
  if (this->mixes.size() > 0 && timePosition.playing && lock.try_lock()) {
    if (this->currentSampleRate == sampleRate) {
      for (uint32_t j = 0; j < mixes.size(); ++j) {
        if (j >= this->outputMap.size()) {
          break;
        }
        if (j >= this->tracks.size()) {
          break;
        }
        const auto &mapping = outputMap[j];
        const auto &left = mixes[j].first;
        const auto &right = mixes[j].second;

        const auto &track = tracks[j];

        for (uint32_t i = 0; i < frames; ++i) {
          auto frame = (i + timePosition.frame);
          if (frame >= left.size()) {
            break;
          }
          if (frame >= right.size()) {
            break;
          }
          auto fadedLeft = left[frame] * Utils::dbToMultiplier(track.volume);
          auto fadedRight = right[frame] * Utils::dbToMultiplier(track.volume);
          if (track.pan < 0) {
            fadedRight *= 1 + (track.pan / 100.0);
          } else if (track.pan > 0) {
            fadedLeft *= 1 - (track.pan / 100.0);
          }
          for (uint32_t k = 0; k < DISTRHO_PLUGIN_NUM_OUTPUTS; ++k) {
            if (mapping.first[k] && frame < left.size()) {
              if (outputs[k][i] > FLT_MAX - fadedLeft) {
                outputs[k][i] = FLT_MAX;
              } else if (outputs[k][i] < -FLT_MAX + fadedLeft) {
                outputs[k][i] = -FLT_MAX;
              } else {
                outputs[k][i] += fadedLeft;
              }
            }
            if (mapping.second[k] && frame < right.size()) {
              if (outputs[k][i] > FLT_MAX - fadedRight) {
                outputs[k][i] = FLT_MAX;
              } else if (outputs[k][i] < -FLT_MAX + fadedRight) {
                outputs[k][i] = -FLT_MAX;
              } else {
                outputs[k][i] += fadedRight;
              }
            }
          }
        }
      }
    } else {
      requestResampleMixes(sampleRate);
    }
  }
};

/* --------------------------------------------------------------------------------------------------------
 * Callbacks (optional) */

/**
                               Optional callback to inform the plugin about a
   buffer size change. This function will only be called when the plugin is
   deactivated.
                               @note This value is only a hint!
                                                                 Hosts might
   call run() with a higher or lower number of frames.
 */
void OpenUtauPlugin::bufferSizeChanged(uint32_t newBufferSize) {}

void OpenUtauPlugin::sampleRateChanged(double newSampleRate) {
  requestResampleMixes(newSampleRate);
}
void OpenUtauPlugin::onAccept(OpenUtauPlugin *self,
                              const asio::error_code &error,
                              asio::ip::tcp::socket socket) {
  if (!error) {
    self->willAccept();
    if (!self->connected) {
      self->connected = true;
      self->acceptorThread = std::make_unique<std::jthread>(
          [self, socket = std::make_shared<asio::ip::tcp::socket>(
                     std::move(socket))](std::stop_token st) mutable {
            std::string messageBuffer;
            char buffer[16 * 1024];
            std::promise<std::variant<size_t, asio::error_code>> readPromise;
            auto readFuture = readPromise.get_future();
            bool timeout = false;
            while (!st.stop_requested()) {
              if (!timeout) {
                readPromise =
                    std::promise<std::variant<size_t, asio::error_code>>();
                readFuture = readPromise.get_future();
                socket->async_read_some(
                    asio::buffer(buffer),
                    [&](const asio::error_code &error, size_t len) {
                      if (error) {
                        readPromise.set_value(error);
                      } else {
                        readPromise.set_value(len);
                      }
                    });
              }
              if (st.stop_requested()) {
                break;
              }
              if (readFuture.wait_for(std::chrono::seconds(1)) ==
                  std::future_status::timeout) {
                timeout = true;
              } else {
                timeout = false;
                auto result = readFuture.get();
                if (std::holds_alternative<asio::error_code>(result)) {
                  break;
                }
                auto len = std::get<size_t>(result);
                messageBuffer.append(buffer, len);

                size_t pos;
                while ((pos = messageBuffer.find('\n')) != std::string::npos) {
                  std::string message = messageBuffer.substr(0, pos);
                  messageBuffer.erase(0, pos + 1);
                  if (message == "close") {
                    socket->close();
                    self->connected = false;
                    return;
                  }

                  size_t sep = message.find(' ');
                  std::string header = message.substr(0, sep);
                  std::string payload = message.substr(sep + 1);

                  size_t firstColon = header.find(':');

                  std::string messageType = header.substr(0, firstColon);
                  choc::value::Value value = choc::json::parse(payload);

                  if (messageType == "request") {
                    size_t secondColon = header.find(':', firstColon + 1);

                    std::string messageId = header.substr(
                        firstColon + 1, secondColon - firstColon - 1);
                    std::string requestType = header.substr(secondColon + 1);

                    self->threads[messageId] =
                        std::jthread([self, socket, messageId, requestType,
                                      value](std::stop_token st) mutable {
                          choc::value::Value responseObj =
                              choc::value::createObject("");
                          try {
                            auto response = self->onRequest(requestType, value);
                            responseObj.setMember("success", true);
                            responseObj.setMember("data", response);
                          } catch (std::exception &e) {
                            responseObj.setMember("success", false);
                            responseObj.setMember("error", e.what());
                          }

                          auto responseString = formatMessage(
                              std::format("response:{}", messageId),
                              responseObj);
                          socket->write_some(asio::buffer(responseString));

                          self->threads[messageId].detach();
                          self->threads.erase(messageId);
                        });
                  } else if (messageType == "notification") {
                    std::string notificationType =
                        header.substr(firstColon + 1);
                    auto messageId = uuid::v4::UUID::New().String();
                    self->threads[messageId] =
                        std::jthread([self, socket, messageId, notificationType,
                                      value](std::stop_token st) mutable {
                          self->onNotification(notificationType, value);

                          self->threads[messageId].detach();
                          self->threads.erase(messageId);
                        });
                  }
                }
              }

              auto currentTime = std::chrono::system_clock::now();
              if (currentTime - self->lastPing > std::chrono::seconds(5)) {
                socket->write_some(asio::buffer(
                    formatMessage("ping", choc::value::createObject(""))));
                self->lastPing = currentTime;
              }
            }

            try {
              socket->close();
            } catch (asio::system_error &e) {
              // ignore
            }

            self->connected = false;
          });
    } else {
      socket.close();
    }
  }
}

void OpenUtauPlugin::willAccept() {
  acceptor->async_accept(std::bind(&OpenUtauPlugin::onAccept, this,
                                   std::placeholders::_1,
                                   std::placeholders::_2));
}

void OpenUtauPlugin::initializeNetwork() {
  this->acceptor = std::make_unique<asio::ip::tcp::acceptor>(
      Network::getIoContext()->get_executor(),
      asio::ip::tcp::endpoint(asio::ip::address::from_string("127.0.0.1"), 0));

  auto port = this->acceptor->local_endpoint().port();
  this->port = port;
  updatePluginServerFile();
  willAccept();
}

void OpenUtauPlugin::updatePluginServerFile() {
  std::filesystem::path tempPath = std::filesystem::temp_directory_path();
  std::filesystem::path socketPath = tempPath / "OpenUtau" / "PluginServers" /
                                     std::format("{}.json", this->uuid);
  std::string socketContent = choc::json::toString(
      choc::value::createObject("", "port", port, "name", this->name));

  std::filesystem::create_directories(socketPath.parent_path());
  std::ofstream socketFile(socketPath);
  socketFile << socketContent;
  socketFile.close();

  this->socketPath = socketPath;
}

choc::value::Value OpenUtauPlugin::onRequest(const std::string kind,
                                             const choc::value::Value payload) {
  if (kind == "init") {
    choc::value::Value response =
        choc::value::createObject("", "ustx", this->ustx);
    return response;
  } else if (kind == "updatePartLayout") {
    auto _lock = std::lock_guard(this->partMutex);
    std::map<int, std::vector<Part>> parts;
    std::vector<Part> flatParts;
    std::set<AudioHash> hashes;
    for (const auto &part : payload["parts"]) {
      flatParts.push_back(Part::deserialize(part));
    }
    for (const auto &part : flatParts) {
      if (parts.find(part.trackNo) == parts.end()) {
        parts[part.trackNo] = std::vector<Part>();
      }
      parts[part.trackNo].push_back(part);
      hashes.insert(part.hash);
    }
    {
      auto _lock = std::lock_guard(this->partsMutex);
      this->parts = parts;
    }
    std::set<AudioHash> toRemove;
    std::set<AudioHash> toAdd;
    for (const auto &hash : this->audioBuffers) {
      if (hashes.find(hash.first) == hashes.end()) {
        toRemove.insert(hash.first);
      }
    }
    for (const auto &hash : hashes) {
      if (this->audioBuffers.find(hash) == this->audioBuffers.end()) {
        toAdd.insert(hash);
      }
    }
    for (const auto &hash : toRemove) {
      this->audioBuffers.erase(hash);
    }

    choc::value::Value response = choc::value::createObject("");
    auto missingAudios = choc::value::createEmptyArray();
    for (const auto &hash : toAdd) {
      missingAudios.addArrayElement(std::to_string(hash));
    }
    response.setMember("missingAudios", missingAudios);

    this->requestResampleMixes(this->currentSampleRate);

    return response;
  }

  throw std::runtime_error("Unknown request type");
}
void OpenUtauPlugin::onNotification(const std::string kind,
                                    const choc::value::Value payload) {
  if (kind == "updateUstx") {
    auto ustx = payload["ustx"].get<std::string>();
    auto ustxBase64 = choc::base64::encodeToString(ustx);
    setState("ustx", ustxBase64.c_str());
  } else if (kind == "updateTracks") {
    auto _lock = std::lock_guard(this->tracksMutex);
    auto tracks = std::vector<Structures::Track>();
    for (const auto &track : payload["tracks"]) {
      tracks.push_back(Structures::Track::deserialize(track));
    }

    this->tracks = tracks;
    syncMapping();
  } else if (kind == "updateAudio") {
    {
      auto _lock = std::lock_guard(this->audioBuffersMutex);

      std::map<AudioHash, std::vector<float>> audioBuffers;
      payload["audios"].visitObjectMembers(
          [&](std::string_view key, const choc::value::ValueView &value) {
            auto hash = std::stoul(std::string(key));
            std::string encoded = value.get<std::string>();
            auto decoded = Utils::unBase64ToVector(encoded);
            auto decompressed =
                Utils::gunzip((char *)decoded.data(), decoded.size());
            std::vector<float> audio((float *)decompressed.data(),
                                     (float *)decompressed.data() +
                                         decompressed.size() / sizeof(float));
            audioBuffers[hash] = audio;
          });

      this->audioBuffers = audioBuffers;
    }

    this->requestResampleMixes(this->currentSampleRate);
  }
}

void OpenUtauPlugin::syncMapping() {
  {
    auto _lock = std::unique_lock(this->mixMutex);

    auto tracks = this->tracks;
    auto outputMap = this->outputMap;
    if (tracks.size() < outputMap.size()) {
      outputMap.resize(tracks.size());
    } else if (tracks.size() > outputMap.size()) {
      for (size_t i = outputMap.size(); i < tracks.size(); ++i) {
        auto leftChannel = std::bitset<DISTRHO_PLUGIN_NUM_OUTPUTS>();
        auto rightChannel = std::bitset<DISTRHO_PLUGIN_NUM_OUTPUTS>();
        leftChannel[0] = true;
        rightChannel[1] = true;

        outputMap.push_back({leftChannel, rightChannel});
      }
    }

    this->outputMap = outputMap;
  }
  setState("mapping", Structures::serializeOutputMap(outputMap).c_str());
}

void OpenUtauPlugin::requestResampleMixes(double newSampleRate) {
  auto uuid = uuid::v4::UUID::New().String();
  this->threads[uuid] = std::jthread([this, uuid, newSampleRate]() {
    this->resampleMixes(newSampleRate);
    this->threads[uuid].detach();
    this->threads.erase(uuid);
  });
}

void OpenUtauPlugin::resampleMixes(double newSampleRate) {
  auto _lock = std::unique_lock(this->mixMutex);
  auto _lock2 = std::shared_lock(this->audioBuffersMutex);
  auto _lock3 = std::shared_lock(this->partsMutex);

  std::vector<std::pair<std::vector<float>, std::vector<float>>> mixes;
  for (const auto &parts : this->parts) {
    std::vector<float> resampledLeft;
    std::vector<float> resampledRight;
    auto maxRightMs = std::max_element(
        parts.second.begin(), parts.second.end(),
        [](const Part &a, const Part &b) { return a.endMs < b.endMs; });
    resampledLeft.resize((size_t)(maxRightMs->endMs / 1000.0 * newSampleRate) +
                         1);
    resampledRight.resize((size_t)(maxRightMs->endMs / 1000.0 * newSampleRate) +
                          1);
    for (const auto &part : parts.second) {
      auto startFrame = (size_t)(part.startMs / 1000.0 * newSampleRate);
      auto endFrame = (size_t)(part.endMs / 1000.0 * newSampleRate);
      auto rate = 44100.0 / newSampleRate;
      if (audioBuffers.find(part.hash) == audioBuffers.end()) {
        continue;
      }
      auto &buffer = audioBuffers[part.hash];

      for (size_t i = startFrame; i < endFrame; ++i) {
        auto frame = (size_t)(i * rate);
        auto leftFrameLeft = frame * 2;
        auto leftFrameRight = frame * 2 + 2;
        auto rightFrameLeft = frame * 2 + 1;
        auto rightFrameRight = frame * 2 + 3;
        auto lerp = (i * rate) - frame;
        if (rightFrameRight >= buffer.size()) {
          break;
        }
        auto left =
            (1 - lerp) * buffer[leftFrameLeft] + lerp * buffer[leftFrameRight];
        auto right = (1 - lerp) * buffer[rightFrameLeft] +
                     lerp * buffer[rightFrameRight];
        if (resampledLeft[i] > FLT_MAX - left) {
          resampledLeft[i] = FLT_MAX;
        } else if (resampledLeft[i] < -FLT_MAX + left) {
          resampledLeft[i] = -FLT_MAX;
        } else {
          resampledLeft[i] += left;
        }
        if (resampledRight[i] > FLT_MAX - right) {
          resampledRight[i] = FLT_MAX;
        } else if (resampledRight[i] < -FLT_MAX + right) {
          resampledRight[i] = -FLT_MAX;
        } else {
          resampledRight[i] += right;
        }
      }
    }

    mixes.push_back({resampledLeft, resampledRight});
  }
  this->mixes = mixes;
  this->currentSampleRate = newSampleRate;
}

std::string
OpenUtauPlugin::formatMessage(const std::string &kind,
                              const choc::value::ValueView &payload) {
  std::string json = choc::json::toString(payload);
  return std::format("{} {}\n", kind, json);
}

bool OpenUtauPlugin::isProcessing() {
  if (this->partMutex.try_lock()) {
    this->partMutex.unlock();
    return false;
  }
  return true;
}

/* ------------------------------------------------------------------------------------------------------------
 * Plugin entry point, called by DPF to create a new plugin instance. */

START_NAMESPACE_DISTRHO
Plugin *createPlugin() { return new OpenUtauPlugin(); }
END_NAMESPACE_DISTRHO

// -----------------------------------------------------------------------------------------------------------
