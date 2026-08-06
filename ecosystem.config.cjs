module.exports = {
  apps: [
    {
      name: 'tinrpc',
      script: './rpc',
      cwd: '/home/mamengchen/mmo/tinrpc/build',
      instances: 1,
      autorestart: true,
      max_memory_restart: '512M',
      env: {
        TINRPC_MONGO_URI: 'mongodb://127.0.0.1:27017',
        TINRPC_MONGO_DB: 'tinrpc',
      },
    },
  ],
};
