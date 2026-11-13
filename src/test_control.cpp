#include "test_control.h"

#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace eck::test_control {
namespace {

// Loopback only, one client at a time, and only when the environment asked for it: nothing outside this PC can reach it and
// no firewall asks, because a listener bound to 127.0.0.1 is not exposed.
constexpr uint16_t kDefaultPort = 8799;

std::mutex g_mutex;
std::deque<Request> g_requests;
std::deque<std::string> g_answers;
std::deque<std::string> g_inbox;  // lines read from the client, waiting for the game thread to take them
Request g_none;
race::PadState g_pad;
bool g_pad_set = false;
void* g_window = nullptr;
bool g_started = false;

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

void CloseSocket(SocketHandle socket) {
#if defined(_WIN32)
  closesocket(socket);
#else
  close(socket);
#endif
}

uint16_t Port() {
  if (const char* value = std::getenv(kPortEnv); value != nullptr && *value != 0) {
    const long port = std::strtol(value, nullptr, 10);
    if (port > 1024 && port < 65536) return uint16_t(port);
  }
  return kDefaultPort;
}

bool SendLine(SocketHandle client, const std::string& line) {
  const std::string text = line + "\n";
  size_t sent = 0;
  while (sent < text.size()) {
    const auto n = send(client, text.data() + sent, int(text.size() - sent), 0);
    if (n <= 0) return false;
    sent += size_t(n);
  }
  return true;
}

// One line in, its answers out, and then the client may stay for the next one.
void HandleLine(SocketHandle client, const std::string& line) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_inbox.push_back(line);
  }
  // The answer is written here, because the game thread must not block on a socket. Five seconds is far past the longest
  // request (a screenshot of a large window).
  for (int tick = 0; tick < 5000; ++tick) {
    std::deque<std::string> answers;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      answers.swap(g_answers);
    }
    bool done = false;
    for (const std::string& answer : answers) {
      if (!SendLine(client, answer)) return;
      if (answer == "end") done = true;
    }
    if (done) return;
#if defined(_WIN32)
    Sleep(1);
#else
    usleep(1000);
#endif
  }
}

void ServeClient(SocketHandle client) {
  REXLOG_INFO("[TEST-CONTROL] client connected");
  std::string buffer;
  char c = 0;
  while (recv(client, &c, 1, 0) > 0) {
    if (c == '\n') {
      if (!buffer.empty()) HandleLine(client, buffer);
      buffer.clear();
    } else if (c != '\r') {
      buffer.push_back(c);
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_answers.clear();
  }
  CloseSocket(client);
  REXLOG_INFO("[TEST-CONTROL] client disconnected");
}

void Serve() {
#if defined(_WIN32)
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    REXLOG_INFO("[TEST-CONTROL] winsock failed to start");
    return;
  }
#endif
  const SocketHandle server = socket(AF_INET, SOCK_STREAM, 0);
  if (server == kInvalidSocket) {
    REXLOG_INFO("[TEST-CONTROL] socket could not be created");
    return;
  }
  int reuse = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof reuse);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(Port());
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 ||
      listen(server, 1) != 0) {
    REXLOG_INFO("[TEST-CONTROL] port {} is taken", Port());
    CloseSocket(server);
    return;
  }
  for (;;) {
    const SocketHandle client = accept(server, nullptr, nullptr);
    if (client == kInvalidSocket) continue;
    // A client that goes away without closing must not stop the next one from connecting, so each has its own thread.
    std::thread(ServeClient, client).detach();
  }
}

#if defined(_WIN32)
void PutPixelBgr(std::vector<uint8_t>& out, int width, int height, const std::vector<uint8_t>& pixels) {
  // A BMP row is bottom-up and padded to four bytes.
  const int row = width * 3;
  const int padding = (4 - row % 4) % 4;
  out.resize(size_t(row + padding) * size_t(height) + 54);
  size_t at = 54;
  for (int y = height - 1; y >= 0; --y) {
    const uint8_t* source = pixels.data() + size_t(y) * size_t(width) * 4;
    for (int x = 0; x < width; ++x) {
      out[at++] = source[x * 4 + 0];  // B
      out[at++] = source[x * 4 + 1];  // G
      out[at++] = source[x * 4 + 2];  // R
    }
    for (int i = 0; i < padding; ++i) out[at++] = 0;
  }
}

void WriteBmpHeader(uint8_t* header, int width, int height, size_t size) {
  const uint32_t row = uint32_t(width * 3);
  const uint32_t padded = row + (4 - row % 4) % 4;
  header[0] = 'B';
  header[1] = 'M';
  const auto put32 = [header](int at, uint32_t value) {
    header[at + 0] = uint8_t(value);
    header[at + 1] = uint8_t(value >> 8);
    header[at + 2] = uint8_t(value >> 16);
    header[at + 3] = uint8_t(value >> 24);
  };
  put32(2, uint32_t(size));
  put32(10, 54);
  put32(14, 40);
  put32(18, uint32_t(width));
  put32(22, uint32_t(height));
  header[26] = 1;
  header[28] = 24;
  put32(34, padded * uint32_t(height));
  put32(38, 2835);
  put32(42, 2835);
}

// The game's own window, found by asking Windows for a visible window of this process: the runtime creates the window, not
// this code, and the paths that could hand the handle over do not all run.
void* FindOwnWindow() {
  struct Search {
    DWORD pid;
    HWND found;
  } search{GetCurrentProcessId(), nullptr};
  EnumWindows(
      [](HWND window, LPARAM param) -> BOOL {
        auto* state = reinterpret_cast<Search*>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != state->pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;
        state->found = window;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&search));
  return search.found;
}
#endif

}  // namespace

bool Enabled() {
  static const bool on = [] {
    const char* value = std::getenv(kEnv);
    return value != nullptr && value[0] == '1';
  }();
  return on;
}

void Start(void* native_window) {
  if (!Enabled()) return;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_started) return;
    g_started = true;
    g_window = native_window;
  }
  REXLOG_INFO("[TEST-CONTROL] listening on 127.0.0.1:{} ({}={})", Port(), kEnv, std::getenv(kEnv));
  std::thread(Serve).detach();
}

namespace {

// Whether a request needs the guest, and therefore the game thread: a console command, a race start, the race state.
bool NeedsGuest(Command command) {
  return command == Command::kConsole || command == Command::kRace || command == Command::kState;
}

// Pops the oldest request that belongs to this thread, leaving the other thread's own in place.
Request TakeWhere(bool guest) {
  if (!Enabled()) return g_none;
  std::lock_guard<std::mutex> lock(g_mutex);
  // Everything the client sent is parsed first, so a malformed line is answered even when the queue looks empty.
  while (!g_inbox.empty()) {
    const std::string line = g_inbox.front();
    g_inbox.pop_front();
    const std::optional<Request> request = Parse(line);
    if (request) {
      g_requests.push_back(*request);
    } else {
      g_answers.push_back("err bad_request " + HelpText());
      g_answers.push_back("end");
    }
  }
  for (auto it = g_requests.begin(); it != g_requests.end(); ++it) {
    if (NeedsGuest(it->command) != guest) continue;
    const Request request = *it;
    g_requests.erase(it);
    return request;
  }
  return g_none;
}

}  // namespace

bool HasPendingGuest() {
  if (!Enabled()) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const Request& request : g_requests) {
    if (NeedsGuest(request.command)) return true;
  }
  return false;
}

// A guest request nobody can serve, because no map is loaded, is answered instead of left hanging.
void DropPendingGuest(const std::string& reason) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto it = g_requests.begin(); it != g_requests.end();) {
    if (!NeedsGuest(it->command)) {
      ++it;
      continue;
    }
    if (it->command == Command::kState) {
      g_answers.push_back("{\"race_state\":-1,\"in_race\":false,\"local_seat\":-1,\"session\":0,\"karts\":[],");
      g_answers.back() += "\"note\":\"" + reason + "\"}";
      g_answers.push_back("ok");
    } else {
      g_answers.push_back("err " + reason);
    }
    g_answers.push_back("end");
    it = g_requests.erase(it);
  }
}

Request TakeForGameThread() { return TakeWhere(true); }

Request TakeForUiThread() { return TakeWhere(false); }

void Answer(const std::string& line) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_answers.push_back(line);
}

void Answered() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_answers.push_back("ok");
  g_answers.push_back("end");
}

void Failed(const std::string& reason) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_answers.push_back("err " + reason);
  g_answers.push_back("end");
}

bool PadOverride(race::PadState& out) {
  if (!Enabled()) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_pad_set) return false;
  out = g_pad;
  return true;
}

void SetPad(const race::PadState& pad) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_pad = pad;
  g_pad_set = true;
}

void ClearPad() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_pad_set = false;
  g_pad = {};
}

bool CaptureTo(const std::string& path) {
#if !defined(_WIN32)
  (void)path;
  return false;
#else
  HWND window = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_window == nullptr) g_window = FindOwnWindow();
    window = static_cast<HWND>(g_window);
  }
  if (window == nullptr) return false;
  RECT rect{};
  if (!GetClientRect(window, &rect)) return false;
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) return false;

  HDC screen = GetDC(nullptr);
  HDC memory = CreateCompatibleDC(screen);
  HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
  HGDIOBJ previous = SelectObject(memory, bitmap);
  // PrintWindow asks the window to draw itself, so a covered window still captures.
  if (!PrintWindow(window, memory, 2 /* PW_RENDERFULLCONTENT */)) {
    BitBlt(memory, 0, 0, width, height, GetDC(window), 0, 0, SRCCOPY);
  }

  std::vector<uint8_t> pixels(size_t(width) * size_t(height) * 4);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof info.bmiHeader;
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // top-down
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  const int got = GetDIBits(memory, bitmap, 0, UINT(height), pixels.data(), &info, DIB_RGB_COLORS);

  SelectObject(memory, previous);
  DeleteObject(bitmap);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  if (got == 0) return false;

  std::vector<uint8_t> file;
  PutPixelBgr(file, width, height, pixels);
  WriteBmpHeader(file.data(), width, height, file.size());

  FILE* out = nullptr;
  if (fopen_s(&out, path.c_str(), "wb") != 0 || out == nullptr) return false;
  const size_t written = fwrite(file.data(), 1, file.size(), out);
  fclose(out);
  REXLOG_INFO("[TEST-CONTROL] shot {} {}x{} bytes={}", path, width, height, written);
  return written == file.size();
#endif
}

}  // namespace eck::test_control
