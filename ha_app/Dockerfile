ARG BUILD_FROM=alpine:3.24
FROM ${BUILD_FROM}
RUN apk add --no-cache nginx jq curl
COPY run.sh check_ota.sh /
RUN chmod +x /run.sh /check_ota.sh
CMD ["/run.sh"]
