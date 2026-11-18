FROM node:22-alpine
WORKDIR /app
COPY server.mjs /app/server.mjs
USER node
ENV BIND=0.0.0.0 PORT=18779
EXPOSE 18779
CMD ["node", "server.mjs"]
