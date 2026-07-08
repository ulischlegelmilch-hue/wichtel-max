# Wichtel-Max-Backend als Container (Render, Railway, Fly.io, ...).
# Baut ein schlankes Node-Image; der Hoster setzt PORT selbst.
FROM node:20-alpine

WORKDIR /app

# Erst nur die Manifeste kopieren -> Docker-Layer-Cache für npm install
COPY package.json package-lock.json* ./
RUN npm install --omit=dev

# Rest der App
COPY . .

# In der Cloud kein MQTT-Broker (TCP 1883 meist dicht) -> Online per HTTP-Heartbeat.
ENV ENABLE_MQTT=0
# Dauerhafte Daten auf ein gemountetes Volume legen (Render/Railway Disk).
ENV STATE_DIR=/data

# PORT kommt vom Hoster (Express liest process.env.PORT).
CMD ["node", "server.js"]
