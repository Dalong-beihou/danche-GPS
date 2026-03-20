/**
 * 服务器配置文件
 * 可以在这里切换不同的HTTPS/HTTP方案
 */

// ===================== 配置模式选择 =====================
// 可选值: 'ngrok', 'localxpose', 'serveo', 'cloudflare', 'https-local', 'http', 'public'
const MODE = 'public'; // 使用公网IP访问

// ===================== 自签名证书配置 =====================
const SSL_CERT = {
  keyFile: 'localhost+2-key.pem',
  certFile: 'localhost+2.pem'
};

// ===================== 各模式配置 =====================
const CONFIGS = {
  ngrok: {
    protocol: 'https',
    port: 443,
    domain: 'superindulgently-nonresidential-lizzette.ngrok-free.dev',
    wsProtocol: 'wss',
    useHttpsServer: false
  },
  localxpose: {
    protocol: 'https',
    port: 443,
    domain: 'your-subdomain.loclx.io',
    wsProtocol: 'wss',
    useHttpsServer: false
  },
  serveo: {
    protocol: 'https',
    port: 443,
    domain: 'your-subdomain.serveo.net',
    wsProtocol: 'wss',
    useHttpsServer: false
  },
  cloudflare: {
    protocol: 'https',
    port: 443,
    domain: 'your-subdomain.trycloudflare.com',
    wsProtocol: 'wss',
    useHttpsServer: false
  },
  'https-local': {
    protocol: 'https',
    port: 5666,
    domain: '127.0.0.1:5666',
    wsProtocol: 'wss',
    useHttpsServer: true
  },
  public: {
    protocol: 'https',
    port: 34232,
    domain: '39.108.125.75:34232',
    wsProtocol: 'wss',
    useHttpsServer: true
  },
  http: {
    protocol: 'http',
    port: 5666,
    domain: '127.0.0.1:5666',
    wsProtocol: 'ws',
    useHttpsServer: false
  }
};

// 当前配置
const currentConfig = CONFIGS[MODE];

module.exports = {
  MODE,
  ...currentConfig,
  SSL_CERT,
  // 本地服务器端口
  localPort: 5666,
  // 获取完整公网URL
  getPublicUrl: () => `${currentConfig.protocol}://${currentConfig.domain}`,
  // 获取WebSocket URL
  getWsUrl: () => `${currentConfig.wsProtocol}://${currentConfig.domain}`
};
