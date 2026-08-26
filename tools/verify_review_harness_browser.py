#!/usr/bin/env python3
"""Exercise the real review harness in a headless Firefox Marionette session.

Upstream: a built standalone HTML file and its asset folder, or the embedded
single-file HTML. Downstream: a truthful pass/fail result for the browser-only
M6 contract: mirrored panels, independent state, live divider, viewer opening,
and 1:1 inspection. This uses only Python's standard library so a reviewer
does not need Playwright, Bun, Selenium, or a project runtime dependency.

The script intentionally does not run reconstruction code or compare image
quality. It validates that the distributable review UI can be loaded and used.
On restricted CI/container environments Firefox may be unable to start; the
error is reported as an environmental blocker instead of being treated as a
pass.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
from typing import Any


class MarionetteError(RuntimeError):
    """Identify a browser-protocol failure separately from a UI assertion."""


class MarionetteCommandError(MarionetteError):
    """Identify a command rejected by the loaded page or browser protocol."""


class HarnessContractError(RuntimeError):
    """Identify a loaded page that fails the review-harness contract."""


class Marionette:
    """Minimal length-prefixed Marionette client for this self-contained check."""

    def __init__(self, host: str = "127.0.0.1", port: int = 2828) -> None:
        self.socket = socket.create_connection((host, port), timeout=5)
        self.socket.settimeout(10)
        self.next_id = 1
        self.session_id: str | None = None
        self._read_frame()  # Firefox sends a greeting before accepting commands.

    def _read_frame(self) -> Any:
        """Read one Marionette length-prefixed JSON response from Firefox."""

        prefix = b""
        while b":" not in prefix:
            chunk = self.socket.recv(1)
            if not chunk:
                raise MarionetteError("Firefox closed the Marionette socket")
            prefix += chunk
        length = int(prefix[:-1])
        payload = b""
        while len(payload) < length:
            chunk = self.socket.recv(length - len(payload))
            if not chunk:
                raise MarionetteError("Firefox truncated a Marionette response")
            payload += chunk
        message = json.loads(payload)
        # The initial Marionette greeting is an object; command responses use
        # the array envelope below. Both are part of the wire protocol.
        if isinstance(message, dict):
            return message
        if message[0] == 1 and message[2] not in (None, "null"):
            raise MarionetteCommandError(f"{message[2]}: {message[3]}")
        return message

    def command(self, name: str, params: dict[str, Any]) -> Any:
        """Send one WebDriver/Marionette command and return its result."""

        command_id = self.next_id
        self.next_id += 1
        if self.session_id is not None:
            params = {**params, "sessionId": self.session_id}
        payload = json.dumps([0, command_id, name, params]).encode()
        self.socket.sendall(str(len(payload)).encode() + b":" + payload)
        response = self._read_frame()
        if response[1] != command_id:
            raise MarionetteError(f"unexpected response id {response[1]} for {command_id}")
        return response[3]

    def start(self) -> None:
        """Create a WebDriver session with no browser-specific capabilities."""

        result = self.command("WebDriver:NewSession", {"capabilities": {"alwaysMatch": {}}})
        self.session_id = result["sessionId"]

    def script(self, source: str) -> Any:
        """Run JavaScript in the loaded page and return its JSON-compatible value."""

        result = self.command(
            "WebDriver:ExecuteScript",
            {"script": source, "args": []},
        )
        return result["value"]

    def close(self) -> None:
        """Close the protocol socket after the caller has ended the session."""

        self.socket.close()


def wait_for_port(process: subprocess.Popen[bytes], timeout: float = 15) -> None:
    """Wait for Firefox's Marionette listener or surface its startup failure."""

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise MarionetteError(f"Firefox exited before Marionette started (exit {process.returncode})")
        try:
            with socket.create_connection(("127.0.0.1", 2828), timeout=0.2):
                return
        except OSError:
            time.sleep(0.1)
    raise MarionetteError("Firefox did not open Marionette port 2828")


def validate_page(client: Marionette, url: str) -> dict[str, Any]:
    """Load one artifact and exercise its visible review interactions."""

    client.command("WebDriver:Navigate", {"url": url})
    deadline = time.monotonic() + 20
    snapshot: dict[str, Any] = {}
    while time.monotonic() < deadline:
        snapshot = client.script(
            """
            return {
              ready: document.readyState,
              panels: document.querySelectorAll('.panel').length,
              assets: document.getElementById('asset-count')?.textContent || '',
              loaded: [...document.querySelectorAll('.compare-stage img')].every(img => img.complete && img.naturalWidth > 0),
              errors: window.__tforgeReviewErrors || []
            };
            """
        )
        if snapshot["ready"] == "complete" and snapshot["panels"] == 2 and snapshot["loaded"]:
            break
        time.sleep(0.25)
    if snapshot.get("panels") != 2 or not snapshot.get("loaded"):
        raise HarnessContractError(f"page did not become usable: {snapshot}")

    def step(name: str, source: str) -> Any:
        """Add the interaction phase to failures so browser crashes are localizable."""

        try:
            return client.script(source)
        except MarionetteCommandError as error:
            raise HarnessContractError(f"{name}: {error}") from error
        except (OSError, TimeoutError) as error:  # Marionette may close the socket on browser crash.
            raise MarionetteError(f"{name}: {error}") from error

    result = step(
        "independent selectors",
        """
        const panels = [...document.querySelectorAll('.panel')];
        const selectorCounts = panels.map(panel => panel.querySelectorAll('.selectors .state-buttons').length);
        const controlStructure = panels.map(panel => ({
          buttons: [...panel.querySelectorAll('.state-buttons')].map(group => ({
            label: group.closest('.field')?.querySelector('label')?.textContent || '',
            options: [...group.querySelectorAll('button')].map(button => ({text: button.textContent, disabled: button.disabled}))
          }))
        }));
        const controlSkeleton = controlStructure.map(structure => ({
          buttons: structure.buttons.map(control => control.label)
        }));
        const initialHash = location.hash;
        const initialLeft = document.querySelectorAll('.compare-stage img')[0].src;
        const initialRight = document.querySelectorAll('.compare-stage img')[1].src;
        const leftScene = panels[0].querySelector('.selectors .state-buttons');
        const alternate = [...leftScene.querySelectorAll('button:not(:disabled)')].find(button => button.getAttribute('aria-pressed') !== 'true');
        if (!alternate) throw new Error('left scene has no alternate real-world value');
        alternate.click();
        const finalLeft = document.querySelectorAll('.compare-stage img')[0].src;
        return {
          selectorCounts,
          controlStructure,
          controlSkeleton,
          initialHash,
          changedLeft: finalLeft !== initialLeft && location.hash !== initialHash,
          independent: document.querySelectorAll('.compare-stage img')[1].src === initialRight
        };
        """,
    )
    split = step(
        "live divider",
        """
        const range = document.querySelector('.compare-range');
        range.value = '25';
        range.dispatchEvent(new Event('input', {bubbles: true}));
        return document.querySelector('.compare-stage').style.getPropertyValue('--split');
        """,
    )
    opened = step(
        "open enlarged viewer",
        """
        document.querySelector('.compare-stage').dispatchEvent(new MouseEvent('click', {bubbles: true}));
        return !document.getElementById('viewer').hidden;
        """,
    )
    pixel_status = step(
        "1:1 pixel mode",
        """
        document.querySelector('[data-view="pixel"]').click();
        return document.getElementById('viewer-status').textContent;
        """,
    )
    zoom_status = step(
        "zoom in",
        """
        document.querySelector('[data-view="in"]').click();
        return document.getElementById('viewer-status').textContent;
        """,
    )
    lens_state = step(
        "lens toggle and Ctrl-click exit",
        """
        document.querySelector('[data-view="fit"]').click();
        const toggle = document.getElementById('lens-toggle');
        toggle.click();
        const enabled = toggle.getAttribute('aria-pressed') === 'true' &&
          !document.querySelector('.magnifier').hidden;
        document.querySelector('.viewer-compare').dispatchEvent(
          new MouseEvent('click', {bubbles: true, ctrlKey: true})
        );
        const disabled = toggle.getAttribute('aria-pressed') === 'false' &&
          document.querySelector('.magnifier').hidden;
        return {enabled, disabled};
        """,
    )
    pan_position = step(
        "pixel-viewer pan",
        """
        const viewerStage = document.getElementById('viewer-stage');
        viewerStage.scrollLeft = 80;
        viewerStage.scrollTop = 60;
        return {left: viewerStage.scrollLeft, top: viewerStage.scrollTop};
        """,
    )
    fit_status = step(
        "fit-to-view",
        """
        document.querySelector('[data-view="fit"]').click();
        return document.getElementById('viewer-status').textContent;
        """,
    )
    step("close enlarged viewer", "document.querySelector('.viewer-close').click(); return true;")
    result.update(
        {
            "equalSelectors": result["selectorCounts"][0] == result["selectorCounts"][1],
            "changedLeft": result["changedLeft"],
            "split": split,
            "opened": opened,
            "pixelStatus": pixel_status,
            "zoomStatus": zoom_status,
            "oneToOne": "100% / 1:1 highest canvas" in pixel_status,
            "zoomed": "150% highest canvas" in zoom_status,
            "lens": lens_state == {"enabled": True, "disabled": True},
            "panPosition": pan_position,
            "panned": pan_position["left"] > 0 and pan_position["top"] > 0,
            "fitStatus": fit_status,
            "fitToView": fit_status.startswith("Fit to view"),
            "finalHash": client.script("return location.hash;"),
            "assetText": client.script("return document.getElementById('asset-count').textContent;"),
        }
    )
    result["equalSelectors"] = result["controlSkeleton"][0] == result["controlSkeleton"][1]
    saved_state = client.script(
        """
        return {
          hash: location.hash,
          images: [...document.querySelectorAll('.compare-stage img')].map(img => img.src),
          selections: [...document.querySelectorAll('.panel')].map(panel => [...panel.querySelectorAll('.state-button[aria-pressed="true"]')].map(button => button.textContent)),
          metadata: [...document.querySelectorAll('.panel .result-meta')].map(meta => meta.textContent)
        };
        """
    )
    saved_hash = saved_state["hash"]
    client.command("WebDriver:Navigate", {"url": url + saved_hash})
    restored = {}
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        restored = client.script(
            """
            return {
              ready: document.readyState,
              hash: location.hash,
              panels: document.querySelectorAll('.panel').length,
              loaded: [...document.querySelectorAll('.compare-stage img')].every(img => img.complete && img.naturalWidth > 0),
              images: [...document.querySelectorAll('.compare-stage img')].map(img => img.src),
              selections: [...document.querySelectorAll('.panel')].map(panel => [...panel.querySelectorAll('.state-button[aria-pressed="true"]')].map(button => button.textContent)),
              metadata: [...document.querySelectorAll('.panel .result-meta')].map(meta => meta.textContent)
            };
            """
        )
        if (
            restored["ready"] == "complete"
            and restored["panels"] == 2
            and restored["hash"] == saved_hash
            and restored["loaded"]
        ):
            break
        time.sleep(0.25)
    result["hashRestored"] = restored.get("hash") == saved_hash and restored.get("panels") == 2
    result["selectionRestored"] = (
        result["hashRestored"]
        and restored.get("images") == saved_state["images"]
        and restored.get("selections") == saved_state["selections"]
        and restored.get("metadata") == saved_state["metadata"]
    )
    required = {
        "equalSelectors": True,
        "independent": True,
        "split": "25%",
        "opened": True,
        "oneToOne": True,
        "zoomed": True,
        "lens": True,
        "panned": True,
        "fitToView": True,
        "hashRestored": True,
        "selectionRestored": True,
    }
    failures = {key: (result.get(key), expected) for key, expected in required.items() if result.get(key) != expected}
    if failures:
        raise HarnessContractError(f"browser contract failed for {url}: {failures}; result={result}")
    return result


def run(url: str, firefox: str) -> dict[str, Any]:
    """Launch Firefox, validate one URL, and always terminate the child."""

    profile = tempfile.mkdtemp(prefix="tforge-review-firefox-")
    process = subprocess.Popen(
        [
            firefox,
            "--headless",
            "--no-remote",
            "--marionette",
            "--profile",
            profile,
            url,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    try:
        wait_for_port(process)
        client = Marionette()
        try:
            client.start()
            return validate_page(client, url)
        finally:
            client.close()
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        output = process.stdout.read().decode(errors="replace") if process.stdout else ""
        shutil.rmtree(profile, ignore_errors=True)
        if process.returncode not in (0, -15) and output:
            raise MarionetteError(f"Firefox output before exit {process.returncode}: {output[-1000:]}")


def main() -> int:
    """Validate the requested artifact and print machine-readable evidence."""

    parser = argparse.ArgumentParser()
    parser.add_argument("artifact", type=Path)
    parser.add_argument("--firefox", default="firefox")
    args = parser.parse_args()
    artifact = args.artifact.resolve()
    if not artifact.is_file():
        parser.error(f"artifact does not exist: {artifact}")
    url = artifact.as_uri()
    try:
        result = run(url, args.firefox)
    except HarnessContractError as error:
        print(json.dumps({"status": "failed", "artifact": str(artifact), "reason": str(error)}))
        return 1
    except (MarionetteError, OSError) as error:
        print(json.dumps({"status": "blocked", "artifact": str(artifact), "reason": str(error)}))
        return 2
    print(json.dumps({"status": "passed", "artifact": str(artifact), "result": result}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
