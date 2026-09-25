package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"strings"
	"time"
)

// This fixture uses net/http's actual request parser and connection hijacker.
// It echoes frames through a small chunk buffer so the test exercises a Go
// application behind the proxy without materializing whole WebSocket messages.
func serveWS(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet || r.URL.RequestURI() != "/alternate/rewritten?route=ws" ||
		r.Host != "public.example" || r.Header.Get("Origin") != "https://public.example" ||
		r.Header.Get("X-Director") != "websocket" ||
		!strings.EqualFold(r.Header.Get("Upgrade"), "websocket") ||
		!strings.Contains(strings.ToLower(r.Header.Get("Connection")), "upgrade") ||
		r.Header.Get("Sec-WebSocket-Version") != "13" ||
		r.Header.Get("Sec-WebSocket-Protocol") != "chat" {
		http.Error(w, "unexpected forwarded WebSocket request", http.StatusBadRequest)
		return
	}
	key := r.Header.Get("Sec-WebSocket-Key")
	decoded, err := base64.StdEncoding.DecodeString(key)
	if err != nil || len(decoded) != 16 {
		http.Error(w, "invalid WebSocket key", http.StatusBadRequest)
		return
	}
	h, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "hijacking unavailable", http.StatusInternalServerError)
		return
	}
	conn, rw, err := h.Hijack()
	if err != nil {
		log.Printf("hijack: %v", err)
		return
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(15 * time.Second))
	accept := sha1.Sum([]byte(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
	_, err = fmt.Fprintf(rw, "HTTP/1.1 101 Switching Protocols\r\n"+
		"Upgrade: websocket\r\nConnection: Upgrade\r\n"+
		"Sec-WebSocket-Accept: %s\r\nSec-WebSocket-Protocol: chat\r\n\r\n",
		base64.StdEncoding.EncodeToString(accept[:]))
	if err == nil {
		err = rw.Flush()
	}
	if err == nil {
		err = echoFrames(rw)
	}
	if err != nil && err != io.EOF {
		log.Printf("WebSocket echo: %v", err)
	}
}

func echoFrames(rw *bufio.ReadWriter) error {
	var header [8]byte
	var mask [4]byte
	var chunk [4096]byte
	for {
		if _, err := io.ReadFull(rw, header[:2]); err != nil {
			return err
		}
		if header[0]&0x80 == 0 || header[1]&0x80 == 0 {
			return fmt.Errorf("expected final masked frame")
		}
		opcode := header[0] & 0x0f
		if opcode != 1 && opcode != 2 && opcode != 8 {
			return fmt.Errorf("unexpected opcode %d", opcode)
		}
		length := uint64(header[1] & 0x7f)
		if length == 126 {
			if _, err := io.ReadFull(rw, header[:2]); err != nil {
				return err
			}
			length = uint64(binary.BigEndian.Uint16(header[:2]))
		} else if length == 127 {
			if _, err := io.ReadFull(rw, header[:8]); err != nil {
				return err
			}
			length = binary.BigEndian.Uint64(header[:8])
		}
		if length > 32768 || (opcode == 8 && length > 125) {
			return fmt.Errorf("oversized frame %d", length)
		}
		if _, err := io.ReadFull(rw, mask[:]); err != nil {
			return err
		}
		if length < 126 {
			header[0], header[1] = 0x80|opcode, byte(length)
			if _, err := rw.Write(header[:2]); err != nil {
				return err
			}
		} else {
			header[0], header[1] = 0x80|opcode, 126
			binary.BigEndian.PutUint16(header[2:4], uint16(length))
			if _, err := rw.Write(header[:4]); err != nil {
				return err
			}
		}
		var offset uint64
		for offset < length {
			n := len(chunk)
			if uint64(n) > length-offset {
				n = int(length - offset)
			}
			if _, err := io.ReadFull(rw, chunk[:n]); err != nil {
				return err
			}
			for i := 0; i < n; i++ {
				chunk[i] ^= mask[(offset+uint64(i))%4]
			}
			if _, err := rw.Write(chunk[:n]); err != nil {
				return err
			}
			if err := rw.Flush(); err != nil {
				return err
			}
			offset += uint64(n)
		}
		if err := rw.Flush(); err != nil {
			return err
		}
		if opcode == 8 {
			return nil
		}
	}
}

func main() {
	if len(os.Args) != 2 {
		log.Fatal("usage: proxy_go_ws_origin address")
	}
	listener, err := net.Listen("tcp", os.Args[1])
	if err != nil {
		log.Fatal(err)
	}
	server := &http.Server{Handler: http.HandlerFunc(serveWS), ReadHeaderTimeout: 5 * time.Second}
	if err := server.Serve(listener); err != nil && err != http.ErrServerClosed {
		log.Fatal(err)
	}
}
