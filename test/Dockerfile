FROM alpine:3.22
RUN apk add --no-cache build-base alsa-lib-dev python3 py3-numpy chrony libcap-utils
WORKDIR /src
COPY . .
RUN make
CMD ["python3", "test/chrony_integration.py"]
