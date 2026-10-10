FROM python:3.12-slim
WORKDIR /app
COPY server.py .
ENV BB_CRASH_DATA=/data BB_CRASH_HOST=0.0.0.0 PORT=8787
RUN mkdir -p /data && chown nobody /data
VOLUME /data
EXPOSE 8787
USER nobody
CMD ["python", "-u", "server.py", "--trust-proxy"]
