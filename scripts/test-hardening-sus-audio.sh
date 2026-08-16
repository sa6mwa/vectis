#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

if [ "${VECTIS_SUS_AUDIO_HARDENING:-0}" != "1" ]; then
  printf '%s\n' \
    'SKIP: set VECTIS_SUS_AUDIO_HARDENING=1 to run cached SUS/audio model hardening'
  exit 0
fi

vectis_bin=${VECTIS_BIN:-"$repo_root/build/debug/vectis"}
if [ ! -x "$vectis_bin" ]; then
  cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-hardening
phase=tool-discovery
status=failed
class=external-tool-unavailable
reason=vectis-binary-missing
artifact=$vectis_bin
next=run make build-debug or set VECTIS_BIN to a built vectis executable
PKT_DIAGNOSTIC_END
EOF
  exit 2
fi

if ! command -v curl >/dev/null 2>&1; then
  cat >&2 <<'EOF'
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-hardening
phase=tool-discovery
status=failed
class=external-tool-unavailable
reason=curl-missing
next=install curl or disable VECTIS_SUS_AUDIO_HARDENING
PKT_DIAGNOSTIC_END
EOF
  exit 2
fi

if ! command -v perl >/dev/null 2>&1; then
  cat >&2 <<'EOF'
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-hardening
phase=tool-discovery
status=failed
class=external-tool-unavailable
reason=perl-missing
next=install perl or set VECTIS_SUS_AUDIO_HARDENING_EXPECTED_TEXT explicitly
PKT_DIAGNOSTIC_END
EOF
  exit 2
fi

cache_root=${VECTIS_SUS_AUDIO_HARDENING_CACHE:-"${XDG_CACHE_HOME:-"$HOME/.cache"}/vectis/hardening/sus-audio"}
audio_url=${VECTIS_SUS_AUDIO_HARDENING_AUDIO_URL:-"https://pkt.systems/trajectory/assets/narration/intro/intro.mp3"}
index_url=${VECTIS_SUS_AUDIO_HARDENING_INDEX_URL:-"https://pkt.systems/trajectory/index.html"}
model_name=${VECTIS_SUS_AUDIO_HARDENING_MODEL:-tiny}
audio_path="$cache_root/intro.mp3"
index_path="$cache_root/index.html"
model_cache="$cache_root/models"

download_atomic() {
  url=$1
  path=$2
  tmp="$path.tmp.$$"

  if [ -s "$path" ]; then
    return 0
  fi
  mkdir -p "$(dirname "$path")"
  rm -f "$tmp"
  if ! curl -fL --retry 3 --connect-timeout 30 --output "$tmp" "$url"; then
    rm -f "$tmp"
    return 1
  fi
  mv "$tmp" "$path"
}

extract_expected_text() {
  perl -0ne '
    if (/<div class="intro-crawl-track">.*?<p>(.*?)<\/p>/s) {
      $t = $1;
      $t =~ s/<[^>]+>//g;
      $t =~ s/&lt;/</g;
      $t =~ s/&gt;/>/g;
      $t =~ s/&amp;/\&/g;
      $t =~ s/\s+/ /g;
      $t =~ s/^\s+|\s+$//g;
      if ($t =~ /^(.*?\.)/) {
        $t = $1;
      }
      print $t;
    }
  ' "$1"
}

download_atomic "$index_url" "$index_path"
download_atomic "$audio_url" "$audio_path"
mkdir -p "$model_cache"

expected=${VECTIS_SUS_AUDIO_HARDENING_EXPECTED_TEXT:-}
if [ -z "$expected" ]; then
  expected=$(extract_expected_text "$index_path")
fi
if [ -z "$expected" ]; then
  cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-hardening
phase=sus-audio-fixture
status=failed
class=test-fixture
reason=expected-text-empty
artifact=$index_path
next=set VECTIS_SUS_AUDIO_HARDENING_EXPECTED_TEXT or inspect the configured index fixture
PKT_DIAGNOSTIC_END
EOF
  exit 2
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/vectis-sus-audio-hardening.XXXXXX")
cleanup_work() {
  rm -rf "$work"
}
trap cleanup_work EXIT INT TERM

cat >"$work/hardening-sus-audio.lua" <<'LUA'
local audio = require("audio")
local sus = require("sus")

local audio_path = assert(os.getenv("VECTIS_SUS_AUDIO_HARDENING_AUDIO_PATH"))
local model_cache = assert(os.getenv("VECTIS_SUS_AUDIO_HARDENING_MODEL_CACHE"))
local expected = assert(os.getenv("VECTIS_SUS_AUDIO_HARDENING_EXPECTED_TEXT"))
local model_name = os.getenv("VECTIS_SUS_AUDIO_HARDENING_MODEL") or "tiny"

local function normalize(text)
  local out = {}
  local pending_space = false
  text = string.lower(text or "")
  for i = 1, #text do
    local ch = string.sub(text, i, i)
    if string.match(ch, "%w") then
      if pending_space and #out > 0 then
        out[#out + 1] = " "
      end
      out[#out + 1] = ch
      pending_space = false
    elseif #out > 0 then
      pending_space = true
    end
  end
  return table.concat(out)
end

local function contains_expected(actual, expected_text)
  if string.find(actual, expected_text, 1, true) then
    return true
  end
  local normalized_actual = normalize(actual)
  local normalized_expected = normalize(expected_text)
  return normalized_expected == "" or
      string.find(normalized_actual, normalized_expected, 1, true) ~= nil
end

local status_events = 0
local model = assert(sus.open_cached({
  model = model_name,
  cache_dir = model_cache,
  cpu_only = true,
  preserve_initial_space_after_first_transcriber = true,
  status = function(event)
    status_events = status_events + 1
    assert(type(event.phase) == "number")
    assert(type(event.model) == "string")
    assert(type(event.cache_path) == "string")
    return 0
  end,
}))
assert(status_events > 0)

local progress_calls = 0
local segment_calls = 0
local transcriber = assert(model:create_transcriber({
  threads = tonumber(os.getenv("VECTIS_SUS_AUDIO_HARDENING_THREADS") or "1"),
  cpu_only = true,
  language = os.getenv("VECTIS_SUS_AUDIO_HARDENING_LANGUAGE") or "en",
  timestamps = true,
  progress = function(percent)
    progress_calls = progress_calls + 1
    assert(type(percent) == "number")
    return 0
  end,
  segment = function(segment)
    segment_calls = segment_calls + 1
    assert(type(segment.text) == "string")
    assert(type(segment.text_length) == "number")
    return 0
  end,
}))

local decoder = assert(audio.decoder.open_file({
  path = audio_path,
  encoding = "mp3",
}))
local segmented_events = 0
local text = assert(transcriber:transcribe_audio_decoder_segmented_text(
  decoder,
  {
    mode = "continuous",
    read_frames = tonumber(os.getenv("VECTIS_SUS_AUDIO_HARDENING_READ_FRAMES") or "4096"),
    step_ms = tonumber(os.getenv("VECTIS_SUS_AUDIO_HARDENING_STEP_MS") or "1000"),
    length_ms = tonumber(os.getenv("VECTIS_SUS_AUDIO_HARDENING_LENGTH_MS") or "7000"),
    keep_ms = tonumber(os.getenv("VECTIS_SUS_AUDIO_HARDENING_KEEP_MS") or "1500"),
    threshold = tonumber(os.getenv("VECTIS_SUS_AUDIO_HARDENING_VOX_THRESHOLD") or "0.03"),
    prebuffer_ms = tonumber(os.getenv("VECTIS_SUS_AUDIO_HARDENING_PREBUFFER_MS") or "50"),
    memory_spool_bytes = tonumber(os.getenv("VECTIS_SUS_AUDIO_HARDENING_MEMORY_SPOOL_BYTES") or "65536"),
    max_spool_bytes = tonumber(os.getenv("VECTIS_SUS_AUDIO_HARDENING_MAX_SPOOL_BYTES") or "1073741824"),
    segmented = function(event)
      segmented_events = segmented_events + 1
      assert(type(event.text) == "string")
      assert(type(event.text_length) == "number")
      assert(type(event.step_index) == "number")
      assert(type(event.is_final) == "boolean")
      return 0
    end,
  }))
assert(decoder:close() == true)
assert(type(text) == "string" and #text > 0)
assert(contains_expected(text, expected),
       "transcript did not contain expected text: " .. text)

local revised = assert(transcriber:revised_text())
assert(contains_expected(revised, expected),
       "revised transcript did not contain expected text: " .. revised)

assert(transcriber:close() == true)
assert(model:close() == true)

print("sus_audio_hardening=ok")
print("model=" .. model_name)
print("audio=" .. audio_path)
print("model_cache=" .. model_cache)
print("progress_calls=" .. tostring(progress_calls))
print("segment_calls=" .. tostring(segment_calls))
print("segmented_events=" .. tostring(segmented_events))
LUA

printf '[sus-audio-hardening] audio=%s\n' "$audio_path"
printf '[sus-audio-hardening] model-cache=%s\n' "$model_cache"
printf '[sus-audio-hardening] expected=%s\n' "$expected"

if ! VECTIS_SUS_AUDIO_HARDENING_AUDIO_PATH="$audio_path" \
     VECTIS_SUS_AUDIO_HARDENING_MODEL_CACHE="$model_cache" \
     VECTIS_SUS_AUDIO_HARDENING_EXPECTED_TEXT="$expected" \
     VECTIS_SUS_AUDIO_HARDENING_MODEL="$model_name" \
     "$vectis_bin" "$work/hardening-sus-audio.lua" \
       >"$work/hardening-sus-audio.out" \
       2>"$work/hardening-sus-audio.err"; then
  cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-hardening
phase=sus-audio
status=failed
class=test
reason=sus-audio-hardening-command-failed
artifact=$work/hardening-sus-audio.err
next=inspect cached audio/model fixture, network access, and whisper backend diagnostics
PKT_DIAGNOSTIC_END
EOF
  cat "$work/hardening-sus-audio.err" >&2
  exit 1
fi

if ! grep -Fxq 'sus_audio_hardening=ok' "$work/hardening-sus-audio.out"; then
  cat >&2 <<'EOF'
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-hardening
phase=sus-audio
status=failed
class=test
reason=missing-success-marker
next=inspect SUS/audio hardening output
PKT_DIAGNOSTIC_END
EOF
  cat "$work/hardening-sus-audio.out" >&2
  exit 1
fi

cat "$work/hardening-sus-audio.out"
