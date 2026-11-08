"""Runs ffa_engine.exe for the server (UCI-like protocol, see src/uci.cpp).

Commands used:
    position startpos pgn4 <tokens>   -> info string pgn4 ok movers <rbyg...> | info string pgn4 error ...
    position fen <fen4>               (fallback when the replay doesn't match chess.com's FEN)
    d                                 -> board, Fen: <fen4>, Key: <hex>
    gamestate                         -> gamestate turn <c> status ... points ... over <0|1> reason <r>
    resignwins <c>                    -> resignwins yes|no   (chess.com's "claim win" is resigning)
    go rtime .. gtime .. rinc .. ginc .. | go movetime <ms>   -> bestmove <move> | bestmove resign
    legalmoves                        -> legalmoves <move> <move> ...
"""
import os
import queue
import subprocess
import threading
import time


class EngineError(RuntimeError):
    pass


class EngineClient:
    def __init__(self, exe, options=None, echo=False):
        self.exe = os.path.abspath(exe)
        self.options = dict(options or {})
        self.echo = echo
        self.process = None
        self._lines = None
        self.stale = []  # 'bestmove' lines dropped by the last go()
        self._start()

    # --- process -------------------------------------------------------------------------------------------

    def _start(self):
        if not os.path.exists(self.exe):
            raise EngineError(f"engine not found: {self.exe}")
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        self.process = subprocess.Popen([self.exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True, bufsize=1, creationflags=flags)
        self._lines = queue.Queue()
        threading.Thread(target=self._read, args=(self.process, self._lines), daemon=True).start()
        self.send("uci")
        self.wait_for("uciok", 30)
        for name, value in self.options.items():
            self.send(f"setoption name {name} value {value}")
        self.sync()

    @staticmethod
    def _read(process, lines):
        for line in process.stdout:
            lines.put(line.rstrip("\r\n"))
        lines.put(None)

    def send(self, cmd):
        if self.echo:
            print(f"[ENGINE] > {cmd[:200]}{'...' if len(cmd) > 200 else ''}")
        try:
            self.process.stdin.write(cmd + "\n")
            self.process.stdin.flush()
        except (BrokenPipeError, OSError) as e:
            raise EngineError(f"engine pipe closed ({e})")

    def read_line(self, timeout):
        try:
            line = self._lines.get(timeout=max(0.001, timeout))
        except queue.Empty:
            raise TimeoutError
        if line is None:
            raise EngineError("engine exited")
        return line

    def wait_for(self, prefix, timeout):
        end = time.monotonic() + timeout
        while True:
            remaining = end - time.monotonic()
            if remaining <= 0:
                raise EngineError(f"no '{prefix}' from the engine within {timeout:.0f} s")
            try:
                line = self.read_line(remaining)
            except TimeoutError:
                continue
            if self.echo and line.startswith("info string"):
                print(f"[ENGINE] {line}")
            if line.startswith(prefix):
                return line

    def sync(self, timeout=30):
        self.send("isready")
        self.wait_for("readyok", timeout)

    def restart(self):
        self.close()
        self._start()

    def close(self):
        if self.process and self.process.poll() is None:
            try:
                self.send("quit")
                self.process.wait(timeout=2)
            except Exception:
                self.process.kill()

    # --- commands ------------------------------------------------------------------------------------------

    def new_game(self):
        self.send("ucinewgame")
        self.sync()

    def set_pgn4(self, tokens):
        """Replays PGN4 move tokens. Returns (movers, error): movers is one color letter per token."""
        self.send("position startpos pgn4 " + " ".join(tokens))
        line = self.wait_for("info string pgn4", 30)
        if " ok movers " in line:
            movers = line.split()[-1]
            return ("" if movers == "-" else movers), None
        return None, line.removeprefix("info string pgn4 ").strip()

    def set_fen(self, fen):
        self.send(f"position fen {fen}")
        self.sync()

    def fen(self):
        self.send("d")
        line = self.wait_for("Fen:", 30)
        self.wait_for("Key:", 30)
        return line.removeprefix("Fen:").strip()

    def gamestate(self):
        self.send("gamestate")
        parts = self.wait_for("gamestate", 30).split()
        info = {parts[i]: parts[i + 1] for i in range(1, len(parts) - 1, 2)}
        return {"turn": info["turn"], "status": info["status"].split(","),
                "points": [int(p) for p in info["points"].split(",")], "over": info["over"] == "1"}

    def resign_wins(self, color):
        self.send(f"resignwins {color}")
        return self.wait_for("resignwins", 30).split()[-1] == "yes"

    def legal_moves(self):
        """The legal moves of the current position, in the engine's notation."""
        self.send("legalmoves")
        return self.wait_for("legalmoves", 30).split()[1:]

    def drain(self):
        """Removes and returns every line the engine has printed but nobody has read yet."""
        lines = []
        while True:
            try:
                line = self._lines.get_nowait()
            except queue.Empty:
                return lines
            if line is None:
                self._lines.put(None)  # keep the exit marker for read_line
                return lines
            lines.append(line)

    def go(self, command, timeout_s):
        """Sends a 'go ...' command and returns the best move ('resign' or None for '(none)').
        Lines left over from earlier commands are dropped first, so a stale 'bestmove' can't be taken as
        this search's answer."""
        self.stale = [ln for ln in self.drain() if ln.startswith("bestmove")]
        self.send(command)
        try:
            line = self.wait_for("bestmove", timeout_s)
        except EngineError:
            self.send("stop")
            line = self.wait_for("bestmove", 10)
        move = line.split()[1] if len(line.split()) > 1 else None
        return None if move in (None, "(none)", "0000") else move
