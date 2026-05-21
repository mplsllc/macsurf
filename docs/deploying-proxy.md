# Deploying MacSurf Proxy

MacSurf Proxy is a single binary. It listens for plain HTTP proxy requests, fetches upstream via HTTPS, and returns plain HTTP. No config files, no dependencies.

```
macsurf-proxy [--port 8765] [--auth user:password]
```

Default port is 8765. Auth is optional, if set, clients must send `Proxy-Authorization: Basic` headers.

---

## 1. Local Network (No Install)

Build and run directly. Your Mac connects over LAN.

```sh
cd proxy/
go build -o macsurf-proxy .
./macsurf-proxy --port 8765
```

Or with auth:

```sh
./macsurf-proxy --port 8765 --auth myuser:mypassword
```

Find your machine's LAN IP (`ip addr` or `ifconfig`). On the Mac, configure the proxy as `http://<your-lan-ip>:8765`. That's it.

If you have a firewall, open port 8765:

```sh
sudo ufw allow 8765/tcp
```

---

## 2. VPS with systemd (Ubuntu/Debian)

### Build the binary

```sh
cd proxy/
CGO_ENABLED=0 go build -o macsurf-proxy .
```

### Create the user

```sh
sudo useradd --system --no-create-home --shell /usr/sbin/nologin macsurf
```

### Install the binary

```sh
sudo cp macsurf-proxy /usr/local/bin/
sudo chmod 755 /usr/local/bin/macsurf-proxy
```

### Write the environment file

```sh
sudo mkdir -p /etc/macsurf-proxy
sudo tee /etc/macsurf-proxy/env > /dev/null <<'EOF'
MACSURF_PROXY_ARGS=--port 8765 --auth myuser:mypassword
EOF
sudo chmod 600 /etc/macsurf-proxy/env
```

Change `myuser:mypassword` to real credentials. If you don't want auth, use:

```
MACSURF_PROXY_ARGS=--port 8765
```

### Install the service

```sh
sudo cp macsurf-proxy.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now macsurf-proxy
```

### Verify

```sh
sudo systemctl status macsurf-proxy
journalctl -u macsurf-proxy -f
curl -x http://myuser:mypassword@localhost:8765 http://example.com
```

### Firewall

```sh
sudo ufw allow 8765/tcp
```

For production, put this behind a VPN or restrict source IPs. An open proxy on the internet will get abused.

---

## 3. Docker

### Build

```sh
cd proxy/
docker build -t macsurf-proxy .
```

### Run

```sh
docker run -d --name macsurf-proxy -p 8765:8765 macsurf-proxy --port 8765 --auth myuser:mypassword
```

Without auth:

```sh
docker run -d --name macsurf-proxy -p 8765:8765 macsurf-proxy
```

### Verify

```sh
curl -x http://myuser:mypassword@localhost:8765 http://example.com
```

### Logs

```sh
docker logs -f macsurf-proxy
```

---

## 4. Classilla Configuration (Mac OS 9)

Classilla is a maintained Mozilla fork for Mac OS 9 that supports HTTP proxies. Use it today to validate MacSurf Proxy before MacSurf Browser exists.

### Step by step

1. Open **Classilla**
2. Go to **Edit > Preferences**
3. In the left sidebar, expand **Advanced** and select **Proxies**
4. Select **Manual proxy configuration**
5. In the **HTTP Proxy** field, enter your proxy machine's IP address (e.g. `192.168.1.50`)
6. In the **Port** field next to it, enter `8765`
7. Check **Use this proxy server for all protocols**
8. Leave **No Proxy for** set to `localhost, 127.0.0.1`
9. Click **OK**

### If you enabled auth on the proxy

Classilla supports HTTP Basic proxy auth. On first request, it will prompt for username and password. Enter the credentials you passed to `--auth`. Check "remember" if offered.

### Testing

Browse to `http://example.com`, this goes through the proxy as plain HTTP and should load immediately.

Browse to `https://macintoshgarden.org`, Classilla sends a CONNECT request, the proxy tunnels TLS end-to-end. The page should load normally with the lock icon.

If pages don't load, verify:
- The proxy is running and reachable from the Mac (`ping <proxy-ip>` from another machine on the same network)
- Port 8765 is open on the proxy machine
- The IP and port in Classilla match exactly
