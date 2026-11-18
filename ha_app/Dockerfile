ARG BUILD_FROM=alpine:3.24
FROM ${BUILD_FROM}
# The app version is part of this step, so each new version installs the packages' newest release
# instead of reusing a cached layer
ARG BUILD_VERSION
RUN echo "app ${BUILD_VERSION}" && apk add --no-cache nginx jq curl
COPY run.sh check_ota.sh /
RUN chmod +x /run.sh /check_ota.sh
CMD ["/run.sh"]
