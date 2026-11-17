#!/bin/sh
set -eu

UPSTREAM="$(jq -r '.upstream // "100.76.61.43:80"' /data/options.json)"

cat > /etc/nginx/nginx.conf <<EOF
daemon off;
user root;
pid /var/run/nginx.pid;
worker_processes 1;
pcre_jit on;
error_log /proc/1/fd/1 error;

events {
    worker_connections 256;
}

http {
    include /etc/nginx/mime.types;
    default_type application/octet-stream;
    access_log off;
    server_tokens off;
    sendfile on;
    tcp_nodelay on;
    tcp_nopush on;
    keepalive_timeout 65;
    client_max_body_size 8M;

    map \$http_upgrade \$connection_upgrade {
        default upgrade;
        ''      close;
    }

    map \$http_cookie \$hub_cookie {
        "~*(?<c>hub_session=[0-9a-f]+)" \$c;
        default "";
    }

    server {
        listen 8099 default_server;

        location / {
            allow 172.30.32.2;
            deny all;

            proxy_pass http://${UPSTREAM};
            proxy_http_version 1.1;
            proxy_pass_request_headers off;
            proxy_set_header Host \$http_host;
            proxy_set_header If-None-Match \$http_if_none_match;
            proxy_set_header X-Hub \$http_x_hub;
            proxy_set_header Cookie \$hub_cookie;
            proxy_set_header Content-Type \$content_type;
            proxy_set_header Content-Length \$content_length;
            proxy_set_header Upgrade \$http_upgrade;
            proxy_set_header Connection \$connection_upgrade;
            proxy_set_header Sec-WebSocket-Key \$http_sec_websocket_key;
            proxy_set_header Sec-WebSocket-Version \$http_sec_websocket_version;
            proxy_set_header Sec-WebSocket-Protocol \$http_sec_websocket_protocol;
            proxy_redirect off;
            proxy_buffering off;
            proxy_request_buffering off;
            proxy_read_timeout 86400s;
            proxy_send_timeout 86400s;
        }
    }
}
EOF

exec nginx
