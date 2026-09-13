FROM debian:12-slim

ARG DEBIAN_FRONTEND=noninteractive

# usbip supplies usbipd/usbip; kmod supplies modprobe/lsmod;
# python3 runs the small Chinese management page without third-party packages.
RUN apt-get update \
    && apt-get install -y --no-install-recommends usbip kmod python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY entrypoint.sh /usr/local/bin/usbip-share-entrypoint
COPY healthcheck.sh /usr/local/bin/usbip-share-healthcheck
COPY web.py /opt/usbip-share/web.py
COPY gateway.py /opt/usbip-share/gateway.py
COPY index.html /opt/usbip-share/index.html
COPY favicon.ico /opt/usbip-share/favicon.ico

RUN chmod 0755 /usr/local/bin/usbip-share-entrypoint /usr/local/bin/usbip-share-healthcheck \
    && mkdir -p /etc/usbip /run/usbip /opt/usbip-share

EXPOSE 5555/tcp

HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
    CMD ["/usr/local/bin/usbip-share-healthcheck"]

ENTRYPOINT ["/usr/local/bin/usbip-share-entrypoint"]
