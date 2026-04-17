#!/usr/bin/env bash
#
# File: .claude/hooks/tts.sh
#
# Auto-language wrapper around play-tts.sh.
#
# Personas:
#   Nova (FR) = fr_FR-siwis-medium Piper voice.
#   Luna (EN) = en_US-lessac-medium Piper voice.
# (SIWIS / Lessac are the datasets the models were trained on — not names.)
#
# Usage:
#   tts.sh "Salut Hortense"        → auto-detects FR → siwis
#   tts.sh "Hello world"            → defaults to EN → lessac
#   tts.sh "..." en                 → force English
#   tts.sh "..." fr                 → force French
#   tts.sh "..." <explicit-voice>   → pass-through voice override
#
# French detection heuristic:
#   - Any French-specific accented character (à â é è ê ë ï î ô ù û ç)
#   - OR common French function words as whole words
#
# If neither fires, default to English.
#
# Project-local wrapper; does not modify AgentVibes' installed scripts.

set -e

# Force UTF-8 locale so the French accent grep below sees multi-byte chars.
# Git Bash / MSYS defaults to C locale, which makes [àâéèêëïîôùûç] match bytes,
# not characters — unreliable on UTF-8 input.
export LC_ALL=C.UTF-8
export LANG=C.UTF-8

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TEXT="$1"
HINT="$2"  # optional: "fr", "en", or explicit voice name

if [[ -z "$TEXT" ]]; then
  echo "Usage: tts.sh <text> [fr|en|<voice-name>]" >&2
  exit 1
fi

FR_VOICE="fr_FR-siwis-medium"
EN_VOICE="en_US-lessac-medium"

# Resolve the voice to use.
case "$HINT" in
  fr|FR|french)    VOICE="$FR_VOICE" ;;
  en|EN|english)   VOICE="$EN_VOICE" ;;
  "")              VOICE="" ;;  # empty → run detection below
  *)               VOICE="$HINT" ;;  # explicit voice name override
esac

if [[ -z "$VOICE" ]]; then
  # Detect French by accented char OR common French function word.
  # The word list is short on purpose — false-positive "la" in English is rare
  # enough, and false-negative French without accents is rare enough, that this
  # works 95% of the time for casual code-review announcements.
  if echo "$TEXT" | grep -qiE "[àâéèêëïîôùûç]"; then
    VOICE="$FR_VOICE"
  elif echo "$TEXT" | grep -qiE "\b(le|la|les|du|des|est|c'est|je|tu|il|elle|on|nous|vous|ça|cette|qui|que|quoi|pas|dans|sur|avec|pour|mais|parce|plus|moins|très|bien|maintenant|voila|voilà|déjà|peut-être|allez|vas-y|fait|faire)\b"; then
    VOICE="$FR_VOICE"
  else
    VOICE="$EN_VOICE"
  fi
fi

# Run synthesis.  play-tts-piper.sh has no Windows playback branch (only
# macOS afplay / Linux paplay-mpv-aplay), so on Git Bash / MSYS the WAV is
# generated but never played.  We capture the output, extract the saved WAV
# path, and play it via PowerShell's Media.SoundPlayer.
OUT="$("$SCRIPT_DIR/play-tts.sh" "$TEXT" "$VOICE" 2>&1)"
echo "$OUT"

# Already played on macOS/Linux natively — skip PowerShell playback there.
case "$(uname -s)" in
  MSYS*|MINGW*|CYGWIN*)
    # Extract the padded WAV path from the output (line: "🎵 Saved to: /path")
    WAV_PATH="$(echo "$OUT" | grep -oE "Saved to: .*\.wav" | sed 's/^Saved to: //')"
    if [[ -z "$WAV_PATH" ]]; then
      # Fallback: last .wav in .claude/audio/
      AUDIO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)/audio"
      WAV_PATH="$(ls -t "$AUDIO_DIR"/tts-padded-*.wav 2>/dev/null | head -n1)"
    fi
    if [[ -n "$WAV_PATH" && -f "$WAV_PATH" ]]; then
      # Convert Git-Bash path to Windows path for PowerShell.
      WIN_PATH="$(cygpath -w "$WAV_PATH" 2>/dev/null || echo "$WAV_PATH")"
      # Play asynchronously so the script returns quickly; Media.SoundPlayer's
      # Play() is async — we flush-sleep-kill the process so it actually plays
      # before exit.  PlaySync() would block the tool call; Play() + short
      # spin lets audio start on Windows mixer.
      powershell.exe -NoProfile -Command "\$p=New-Object Media.SoundPlayer '$WIN_PATH'; \$p.PlaySync()" 2>/dev/null &
      disown 2>/dev/null || true
    fi
    ;;
esac
