FROM node:24-alpine AS build
WORKDIR /app/effects
COPY effects/package*.json ./
RUN npm ci --omit=dev
COPY effects/src ./src
WORKDIR /app/api
COPY api/package*.json ./
RUN npm ci
COPY api/tsconfig.json ./
COPY api/prisma ./prisma
RUN npx prisma generate
COPY api/src ./src
RUN npm run build

FROM node:24-alpine
WORKDIR /app/effects
COPY --from=build /app/effects ./
WORKDIR /app/api
ENV NODE_ENV=production
COPY api/package*.json ./
RUN npm ci --omit=dev
COPY --from=build /app/api/dist ./dist
COPY --from=build /app/api/node_modules/.prisma ./node_modules/.prisma
COPY api/prisma ./prisma
EXPOSE 3000
CMD ["node", "dist/index.js"]
