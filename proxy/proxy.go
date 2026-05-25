package main

import (
	"io"
	"log"
	"net"
	"net/http"
	"strconv"
	"time"
)

type Proxy struct {
	Auth *Credentials
}

// redirectClient follows up to 10 redirects server-side. MacSurf cannot
// follow HTTPS redirects on its own (no TLS on the browser side), so the
// proxy resolves them and returns the final content.
var redirectClient = &http.Client{
	Transport: http.DefaultTransport,
	Timeout:   60 * time.Second,
}

func (p *Proxy) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if p.Auth != nil && !p.Auth.Check(r) {
		w.Header().Set("Proxy-Authenticate", `Basic realm="macsurf-proxy"`)
		http.Error(w, "Proxy authentication required", http.StatusProxyAuthRequired)
		return
	}

	if r.Method == http.MethodConnect {
		p.handleConnect(w, r)
	} else {
		p.handleHTTP(w, r)
	}
}

func (p *Proxy) handleHTTP(w http.ResponseWriter, r *http.Request) {
	if r.URL.Host == "" {
		http.Error(w, "missing host in request", http.StatusBadRequest)
		return
	}

	host := r.URL.Host
	if h, _, err := net.SplitHostPort(host); err == nil {
		host = h
	}

	outReq, err := http.NewRequestWithContext(r.Context(), r.Method, r.URL.String(), r.Body)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadGateway)
		return
	}
	copyHeaders(outReq.Header, r.Header)
	outReq.Header.Del("Proxy-Authorization")
	outReq.Header.Del("Proxy-Connection")

	resp, err := redirectClient.Do(outReq)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadGateway)
		return
	}
	defer resp.Body.Close()

	// fixes93 — buffer the full upstream body and emit a fixed
	// Content-Length so the MacSurf-side keep-alive pool can reuse
	// the front connection. Previously io.Copy(w, resp.Body) made
	// Go's net/http server fall back to Transfer-Encoding: chunked
	// for any streamed response (i.e. anything not pre-buffered by
	// upstream), and MacSurf treats chunked as not-keep-aliveable
	// (no framing to know where the response ends), so >75% of
	// responses closed their socket. Most page resources are well
	// under 10 MB, so full-buffer is safe; the proxy is the only
	// place we can bound the body cheaply.
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadGateway)
		return
	}

	copyHeaders(w.Header(), resp.Header)
	// Strip transfer headers that Go would otherwise re-emit. We
	// want the response framed by Content-Length only.
	w.Header().Del("Transfer-Encoding")
	w.Header().Del("Content-Length")
	w.Header().Set("Content-Length", strconv.Itoa(len(body)))
	// Encourage keep-alive on the front side. Go honours
	// `Connection: keep-alive` from HTTP/1.1 clients by default,
	// but make it explicit in case the upstream injected close.
	w.Header().Set("Connection", "keep-alive")
	w.WriteHeader(resp.StatusCode)
	w.Write(body)
}

func (p *Proxy) handleConnect(w http.ResponseWriter, r *http.Request) {
	dest, err := net.DialTimeout("tcp", r.Host, 10*time.Second)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadGateway)
		return
	}

	hj, ok := w.(http.Hijacker)
	if !ok {
		dest.Close()
		http.Error(w, "hijacking not supported", http.StatusInternalServerError)
		return
	}

	w.WriteHeader(http.StatusOK)

	client, _, err := hj.Hijack()
	if err != nil {
		dest.Close()
		log.Printf("hijack error: %v", err)
		return
	}

	go transfer(dest, client)
	go transfer(client, dest)
}

const idleTimeout = 10 * time.Minute

func transfer(dst, src net.Conn) {
	defer dst.Close()
	defer src.Close()

	buf := make([]byte, 32*1024)
	for {
		src.SetDeadline(time.Now().Add(idleTimeout))
		n, readErr := src.Read(buf)
		if n > 0 {
			dst.SetDeadline(time.Now().Add(idleTimeout))
			if _, writeErr := dst.Write(buf[:n]); writeErr != nil {
				return
			}
		}
		if readErr != nil {
			return
		}
	}
}

func copyHeaders(dst, src http.Header) {
	for k, vv := range src {
		for _, v := range vv {
			dst.Add(k, v)
		}
	}
}
