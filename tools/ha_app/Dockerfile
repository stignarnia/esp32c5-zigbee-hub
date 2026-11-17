ARG BUILD_FROM=alpine:3.24
FROM ${BUILD_FROM}
RUN apk add --no-cache nginx jq
COPY run.sh /run.sh
RUN chmod +x /run.sh
CMD ["/run.sh"]
