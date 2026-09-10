FROM node:20-alpine

WORKDIR /app

COPY package*.json ./
RUN npm ci --only=production

COPY . .

ENV PORT=10000
EXPOSE 10000 3000

CMD ["node", "server.js"]
