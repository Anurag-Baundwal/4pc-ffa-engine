"""chess.com variants bot API client (the same GET endpoints stockfish_4pc and the Chaturaji bot use).

Differences from those copies:
- The token never appears in an error message: requests puts the full URL (with ?token=...) into its
  exceptions, so every error is re-raised as ApiError with the token masked.
- Only state polls are retried automatically. A retried 'play' could send a move twice.
- A play reply saying "it's not your turn" is NOT a rejection: the API answers that way even when it accepts
  the move. The server confirms moves by watching the next states.
"""
import re
from urllib.parse import quote, urljoin

import requests
from requests.adapters import HTTPAdapter
from urllib3.util import Retry

MAIN_SERVER_URL = "https://variants.gcp-prod.chess.com/bot"
TEST_SERVER_URL = "https://variants.gcp-sandbox.chess-platform.com/bot/"

ENDPOINTS = {
    "state": "?token={}",
    "play": "?token={}&play={}",
    "resign": "?token={}&play=R",
    "chat": "?token={}&chat={}",
}


class ApiError(Exception):
    """A failed API call. The message never contains the token."""


def redact(text, token):
    text = str(text)
    if token:
        text = text.replace(token, "***").replace(quote(token), "***")
    return re.sub(r"token=[^&\s'\")]+", "token=***", text)


class Api:
    def __init__(self, url, token, bot_name="?", version="v1.0.0"):
        self.url = url
        self._token = token
        self.version = version
        headers = {"User-Agent": f"chesscom-bot/{version} user:{bot_name}"}
        # polls: retried on transient server errors
        self._poll = requests.Session()
        retries = Retry(total=3, backoff_factor=0.1, status_forcelist=[500, 502, 503, 504], raise_on_status=False,
                        allowed_methods=["GET"])
        self._poll.mount("https://", HTTPAdapter(max_retries=retries))
        self._poll.headers.update(headers)
        # actions (play, resign, chat): never retried automatically
        self._act = requests.Session()
        self._act.mount("https://", HTTPAdapter(max_retries=0))
        self._act.headers.update(headers)

    def _get(self, session, path):
        try:
            response = session.get(urljoin(self.url, path), timeout=5)
            response.raise_for_status()
            return response.json()
        except (requests.RequestException, ValueError) as e:
            raise ApiError(f"{type(e).__name__}: {redact(e, self._token)}") from None

    def get_state(self):
        return self._get(self._poll, ENDPOINTS["state"].format(self._token))

    def play(self, move):
        return self._get(self._act, ENDPOINTS["play"].format(self._token, quote(str(move))))

    def resign(self):
        return self._get(self._act, ENDPOINTS["resign"].format(self._token))

    def chat(self, message):
        return self._get(self._act, ENDPOINTS["chat"].format(self._token, quote(str(message))))
