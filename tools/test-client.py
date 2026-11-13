"""Sends one request to a running game's test channel and prints its answer.

    python -I tools/test-client.py "state"
    python -I tools/test-client.py "pad 0 0 0 255 0"
    python -I tools/test-client.py "shot C:\\tmp\\race.bmp"

The channel is a socket on 127.0.0.1 (8799 unless ECK_TEST_CONTROL_PORT says otherwise), open only
when the game runs with ECK_TEST_CONTROL=1; see docs/online-ui.md. The answer is one or more lines
ending with "end", and "ok" means the request was carried out. The exit code is 0 for ok, 1 when the
channel cannot be reached and 2 when the game refused the request.
"""
import os
import socket
import sys

PORT = int(os.environ.get("ECK_TEST_CONTROL_PORT", "8799"))


def send(request, timeout=15.0):
    try:
        with socket.create_connection(("127.0.0.1", PORT), timeout=3.0) as channel:
            channel.settimeout(timeout)
            channel.sendall((request + "\n").encode())
            answer, buffer = [], b""
            while True:
                chunk = channel.recv(4096)
                if not chunk:
                    break
                buffer += chunk
                while b"\n" in buffer:
                    line, buffer = buffer.split(b"\n", 1)
                    text = line.decode(errors="replace").strip()
                    if not text:
                        continue
                    answer.append(text)
                    if text == "end":
                        for item in answer[:-1]:
                            print(item)
                        return 0 if "ok" in answer else 2
            for item in answer:
                print(item)
            return 0 if "ok" in answer else 2
    except OSError as error:
        print(f"cannot reach 127.0.0.1:{PORT}: {error}")
        return 1


if __name__ == "__main__":
    sys.exit(send(" ".join(sys.argv[1:]) if len(sys.argv) > 1 else "help"))
