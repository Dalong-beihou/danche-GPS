const express = require('express');
const WebSocket = require('ws');
const http = require('http');
const https = require('https');
const bodyParser = require('body-parser');
const rateLimit = require('express-rate-limit');
const fs = require('fs');
const path = require('path');
const serverConfig = require('./server-config');

const app = express();
// 反向代理场景下，正确识别真实客户端IP（也让 rate-limit 更准确）
app.set('trust proxy', 1);

// ===================== 核心配置（使用server-config.js）=====================
const CONFIG_FILE_PATH = path.join(__dirname, 'devices-config.json');
const DEVICE_DATA_FILE = path.join(__dirname, 'device-data.json');
const TRACK_DATA_FILE = path.join(__dirname, 'track-data.json');

const SERVER_PORT = process.env.PORT || serverConfig.localPort;
const PUBLIC_DOMAIN = serverConfig.domain;
const PUBLIC_SERVER_PROTOCOL = serverConfig.protocol;
const PUBLIC_SERVER_PORT = serverConfig.port;
const WS_PROTOCOL = serverConfig.wsProtocol;

// ===================== 全局存储 =====================
const deviceStatus = {};       // 设备实时状态 {id: {lat, lng, vibration, lastOnline, speed, direction, satellite}}
const deviceTracks = {};       // 设备轨迹 {id: {segments: [{hourKey,startTime,endTime,path:[]}, ...], updateTime}}
const ONLINE_TIMEOUT = 30 * 1000;    // 公网访问延长离线判定为30秒（适配网络延迟）
const MAX_HOURLY_SEGMENTS = 3;       // 最多保留近3个小时段的轨迹
const MAX_JUMP_METERS = 5000;        // 过滤异常跳点（>5km 视为异常）
const CLEAN_INTERVAL = 10 * 60 * 1000;// 公网场景10分钟清理一次离线超久设备
const GPS_OFFSET_NORTH_M = 6;
const GPS_OFFSET_EAST_M = -125; // 向东移动125米，修正75米偏差

// 数据缓存配置
const CACHE_CONFIG = {
  writeInterval: 2000, // 批量写入间隔（毫秒）
  maxWriteSize: 100,   // 最大批量写入大小
  deviceDataChanged: false, // 设备数据是否有变更
  trackDataChanged: false,  // 轨迹数据是否有变更
  lastWriteTime: Date.now() // 上次写入时间
};

// ===================== 工具函数 =====================
// 读取配置文件
function readConfigFile() {
  try {
    if (!fs.existsSync(CONFIG_FILE_PATH)) {
      const defaultConfig = {
        devices: [],
        reconnectInterval: 5000, // 公网访问延长重连间隔
        dataRefreshInterval: 2000 // 公网访问延长数据刷新间隔
      };
      fs.writeFileSync(CONFIG_FILE_PATH, JSON.stringify(defaultConfig, null, 2), 'utf8');
      return defaultConfig;
    }
    const content = fs.readFileSync(CONFIG_FILE_PATH, 'utf8');
    return JSON.parse(content);
  } catch (err) {
    console.error('[读取配置文件失败]', err);
    return {
      devices: [],
      reconnectInterval: 5000,
      dataRefreshInterval: 2000
    };
  }
}

// 写入配置文件
function writeConfigFile(config) {
  try {
    fs.writeFileSync(CONFIG_FILE_PATH, JSON.stringify(config, null, 2), 'utf8');
    console.log('[配置文件写入成功]', CONFIG_FILE_PATH);
    return true;
  } catch (err) {
    console.error('[写入配置文件失败]', err);
    return false;
  }
}

// 读取设备实时数据
function readDeviceDataFile() {
  try {
    if (fs.existsSync(DEVICE_DATA_FILE)) {
      const content = fs.readFileSync(DEVICE_DATA_FILE, 'utf8');
      return JSON.parse(content);
    }
    return {};
  } catch (err) {
    console.error('[读取设备数据文件失败]', err);
    return {};
  }
}

// 写入设备实时数据
function writeDeviceDataFile(data) {
  try {
    fs.writeFileSync(DEVICE_DATA_FILE, JSON.stringify(data, null, 2), 'utf8');
    return true;
  } catch (err) {
    console.error('[写入设备数据文件失败]', err);
    return false;
  }
}

// 读取轨迹数据
function readTrackDataFile() {
  try {
    if (fs.existsSync(TRACK_DATA_FILE)) {
      const content = fs.readFileSync(TRACK_DATA_FILE, 'utf8');
      return JSON.parse(content);
    }
    return {};
  } catch (err) {
    console.error('[读取轨迹数据文件失败]', err);
    return {};
  }
}

// 写入轨迹数据
function writeTrackDataFile(data) {
  try {
    fs.writeFileSync(TRACK_DATA_FILE, JSON.stringify(data, null, 2), 'utf8');
    return true;
  } catch (err) {
    console.error('[写入轨迹数据文件失败]', err);
    return false;
  }
}

function applyGpsOffset(lat, lng, gpsValid) {
  if (!Number.isFinite(lat) || !Number.isFinite(lng) || !gpsValid || lat === 0 || lng === 0) {
    return { lat, lng };
  }
  const R = 6378137;
  const latRad = lat * Math.PI / 180;
  const dLat = GPS_OFFSET_NORTH_M / R;
  const dLng = GPS_OFFSET_EAST_M / (R * Math.cos(latRad));
  return {
    lat: lat + dLat * 180 / Math.PI,
    lng: lng + dLng * 180 / Math.PI
  };
}

// 初始化数据（服务启动时加载）
function initData() {
  // 加载设备实时数据
  const savedDeviceData = readDeviceDataFile();
  Object.assign(deviceStatus, savedDeviceData);
  
  // 加载轨迹数据
  const savedTrackData = readTrackDataFile();
  Object.assign(deviceTracks, savedTrackData);
  
  console.log(`[数据初始化] 加载${Object.keys(deviceStatus).length}个设备实时数据，${Object.keys(deviceTracks).length}个设备轨迹数据`);
}

// ===================== 基础配置（适配ngrok HTTPS）====================
app.use(express.static(__dirname));
app.use(express.static('public'));
app.get('/gps-monitor.html', (req, res) => {
  res.sendFile(__dirname + '/gps-monitor.html');
});
app.get('/', (req, res) => {
  res.sendFile(__dirname + '/gps-monitor.html');
});

// 增加请求体大小限制，适配公网传输
app.use(bodyParser.json({ limit: '50kb' }));
app.use(bodyParser.urlencoded({ extended: true, limit: '50kb' }));

// 增强CORS配置，适配HTTPS跨域
app.use((req, res, next) => {
  // 允许所有来源（适配ngrok动态域名）
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization');
  res.setHeader('Access-Control-Max-Age', '86400'); // 24小时缓存跨域配置
  // 处理OPTIONS预检请求
  if (req.method === 'OPTIONS') {
    return res.status(204).end();
  }
  next();
});

// HTTPS环境下的限流优化（放宽限制，适配网络延迟）
const uploadLimiter = rateLimit({
  windowMs: 1000,
  max: 20, // 公网场景放宽到20次/秒
  message: { code: -1, msg: 'HTTPS请求过于频繁，请稍后再试' },
  standardHeaders: true,
  legacyHeaders: false,
  // 跳过本地IP限流，方便调试
  skip: (req) => {
    const ip = req.ip || req.connection.remoteAddress;
    return ip.includes('127.0.0.1') || ip.includes('::1') || ip.includes('192.168') || ip.includes('10.');
  }
});
app.use('/api/upload', uploadLimiter);

// 增加全局错误处理
app.use((err, req, res, next) => {
  console.error('[全局错误]', {
    method: req.method,
    url: req.url,
    headers: req.headers,
    body: req.body,
    error: err.message,
    stack: err.stack
  });
  res.status(500).json({ 
    code: -1, 
    msg: '服务器内部错误',
    timestamp: new Date().toISOString(),
    requestId: Date.now().toString(36) // 添加请求ID，方便排查
  });
});

// ===================== 设备控制接口 =====================
// 控制设备（LED闪烁、蜂鸣器报警）
app.post('/api/control/:id', (req, res) => {
  try {
    const deviceId = req.params.id;
    const { action, duration = 2000 } = req.body;
    
    if (!action || !['alert', 'led', 'buzzer'].includes(action)) {
      return res.status(400).json({ code: -1, msg: '无效的控制指令' });
    }
    
    console.log(`[设备控制] 设备${deviceId} | 动作:${action} | 时长:${duration}ms | 客户端IP:${req.ip}`);
    
    // 在这里可以添加与ESP32的通信逻辑
    // 例如通过MQTT、WebSocket或HTTP请求控制设备
    
    res.status(200).json({
      success: true,
      message: `设备${deviceId}控制指令已发送`,
      action,
      duration
    });
  } catch (err) {
    console.error('[设备控制失败]', { error: err.message, stack: err.stack, clientIp: req.ip });
    res.status(500).json({ success: false, message: '服务器内部错误' });
  }
});

// ===================== 设备配置管理接口（兼容ngrok HTTPS）=====================
// 1. 获取所有设备配置
app.get('/api/config/devices', (req, res) => {
  try {
    const config = readConfigFile();
    res.status(200).json({
      code: 0,
      data: config.devices,
      reconnectInterval: config.reconnectInterval,
      dataRefreshInterval: config.dataRefreshInterval,
      serverInfo: {
        host: 'localhost',
        port: SERVER_PORT,
        publicDomain: PUBLIC_DOMAIN,
        publicProtocol: PUBLIC_SERVER_PROTOCOL,
        publicPort: PUBLIC_SERVER_PORT,
        onlineTimeout: ONLINE_TIMEOUT / 1000 + 's',
        wsProtocol: WS_PROTOCOL,
        wsUrl: `${WS_PROTOCOL}://${PUBLIC_DOMAIN}`
      }
    });
  } catch (err) {
    res.status(500).json({ code: -1, msg: '获取设备配置失败' });
  }
});

// 2. 添加设备配置（支持ngrok HTTPS域名）
app.post('/api/config/device', (req, res) => {
  try {
    const { id, name, ip, color } = req.body;
    // 参数校验（放宽IP校验，支持域名）
    if (!id || !name || !ip || !color) {
      console.warn('[添加设备配置] 参数缺失', { id, name, ip, color });
      return res.status(400).json({ code: -1, msg: '设备信息不能为空' });
    }
    if (/[^A-Za-z0-9_]/.test(id)) {
      console.warn('[添加设备配置] 设备ID格式错误', { id });
      return res.status(400).json({ code: -1, msg: '设备ID仅支持字母、数字、下划线' });
    }

    const config = readConfigFile();
    // 检查ID是否重复
    const exists = config.devices.some(d => d.id === id);
    if (exists) {
      console.warn('[添加设备配置] 设备ID已存在', { id });
      return res.status(400).json({ code: -1, msg: '设备ID已存在' });
    }

    // 添加新设备（支持HTTPS域名）
    config.devices.push({ id, name, ip, color });
    const success = writeConfigFile(config);
    
    if (success) {
      console.log('[添加设备配置成功]', { id, name, ip, color });
      res.status(200).json({ 
        code: 0, 
        msg: '设备添加成功', 
        data: config.devices,
        tip: `${PUBLIC_SERVER_PROTOCOL.toUpperCase()}访问时使用域名：${PUBLIC_DOMAIN}`
      });
    } else {
      console.error('[添加设备配置] 保存配置文件失败');
      res.status(500).json({ code: -1, msg: '保存配置文件失败' });
    }
  } catch (err) {
    console.error('[添加设备配置失败]', {
      error: err.message,
      stack: err.stack,
      body: req.body
    });
    res.status(500).json({ code: -1, msg: '服务器内部错误' });
  }
});

// 3. 修改设备配置
app.put('/api/config/device/:id', (req, res) => {
  try {
    const deviceId = req.params.id;
    const { name, ip, color } = req.body;
    
    if (!name || !ip || !color) {
      return res.status(400).json({ code: -1, msg: '修改信息不能为空' });
    }

    const config = readConfigFile();
    const deviceIndex = config.devices.findIndex(d => d.id === deviceId);
    
    if (deviceIndex === -1) {
      return res.status(404).json({ code: -1, msg: '设备不存在' });
    }

    // 更新设备信息（支持HTTPS域名）
    config.devices[deviceIndex] = {
      ...config.devices[deviceIndex],
      name,
      ip,
      color
    };

    const success = writeConfigFile(config);
    if (success) {
      res.status(200).json({ code: 0, msg: '设备修改成功', data: config.devices });
    } else {
      res.status(500).json({ code: -1, msg: '保存配置文件失败' });
    }
  } catch (err) {
    console.error('[修改设备配置失败]', err);
    res.status(500).json({ code: -1, msg: '服务器内部错误' });
  }
});

// 4. 删除设备配置
app.delete('/api/config/device/:id', (req, res) => {
  try {
    const deviceId = req.params.id;
    const config = readConfigFile();
    
    // 过滤删除设备
    config.devices = config.devices.filter(d => d.id !== deviceId);
    const success = writeConfigFile(config);
    
    if (success) {
      // 清理设备状态和轨迹
      delete deviceStatus[deviceId];
      delete deviceTracks[deviceId];
      writeDeviceDataFile(deviceStatus);
      writeTrackDataFile(deviceTracks);
      res.status(200).json({ code: 0, msg: '设备删除成功', data: config.devices });
    } else {
      res.status(500).json({ code: -1, msg: '保存配置文件失败' });
    }
  } catch (err) {
    console.error('[删除设备配置失败]', err);
    res.status(500).json({ code: -1, msg: '服务器内部错误' });
  }
});

// 5. 批量更新设备配置
app.post('/api/config/devices/batch', (req, res) => {
  try {
    const { devices, reconnectInterval, dataRefreshInterval } = req.body;
    
    // 参数校验
    if (!Array.isArray(devices)) {
      return res.status(400).json({ code: -1, msg: '设备列表必须为数组' });
    }
    if (typeof reconnectInterval !== 'number' || typeof dataRefreshInterval !== 'number') {
      return res.status(400).json({ code: -1, msg: '时间间隔必须为数字' });
    }

    // 构造完整配置（支持HTTPS域名）
    const newConfig = {
      devices: devices.map(device => ({
        id: (device.id || '').trim(),
        name: (device.name || '').trim(),
        ip: (device.ip || '').trim(),
        color: (device.color || '#FF0000').trim()
      })).filter(device => device.id && device.name && device.ip),
      reconnectInterval: reconnectInterval,
      dataRefreshInterval: dataRefreshInterval
    };

    // 写入配置文件
    const success = writeConfigFile(newConfig);
    if (success) {
      res.status(200).json({ 
        code: 0, 
        msg: '批量更新设备配置成功', 
        data: newConfig.devices,
        serverPort: SERVER_PORT,
        publicUrl: `${PUBLIC_SERVER_PROTOCOL}://${PUBLIC_DOMAIN}`
      });
    } else {
      res.status(500).json({ code: -1, msg: '写入配置文件失败' });
    }
  } catch (err) {
    console.error('[批量更新设备配置失败]', err);
    res.status(500).json({ code: -1, msg: '服务器内部错误' });
  }
});

// ===================== ESP32数据交互接口（HTTPS优化）=====================
// 数据上报接口（增强：支持HTTPS延迟、重试机制）
app.post('/api/upload', uploadLimiter, (req, res) => {
  try {
    const { id, lat = 0, lng = 0, wifi_lat = null, wifi_lng = null, wifi_valid = false, wifi_age_ms = null, ble_lat = null, ble_lng = null, ble_valid = false, ble_age_ms = null, vibration = 0, speed = 0, direction = "--", satellite = 0, gps_valid = false, outside_campus = false, temp = null, hum = null } = req.body;
    
    if (!id || !/^[A-Za-z0-9_]{3,20}$/.test(id)) {
      console.warn('[数据上报] 设备ID格式错误', { id, clientIp: req.ip });
      return res.status(400).json({ 
        code: -1, 
        msg: '设备ID格式错误（仅支持3-20位字母/数字/下划线）' 
      });
    }

    const validLat = parseFloat(lat);
    const validLng = parseFloat(lng);
    const validVibration = parseInt(vibration, 10);
    const validSpeed = parseFloat(speed);
    const validSatellite = parseInt(satellite, 10);
    const validGpsValid = Boolean(gps_valid);
    const validWifiLat = wifi_lat === null || wifi_lat === undefined ? null : parseFloat(wifi_lat);
    const validWifiLng = wifi_lng === null || wifi_lng === undefined ? null : parseFloat(wifi_lng);
    const validWifiValid = Boolean(wifi_valid);
    const validWifiAge = wifi_age_ms === null || wifi_age_ms === undefined ? null : parseInt(wifi_age_ms, 10);
    // 蓝牙定位数据解析
    const validBleLat = ble_lat === null || ble_lat === undefined ? null : parseFloat(ble_lat);
    const validBleLng = ble_lng === null || ble_lng === undefined ? null : parseFloat(ble_lng);
    const validBleValid = Boolean(ble_valid);
    const validBleAge = ble_age_ms === null || ble_age_ms === undefined ? null : parseInt(ble_age_ms, 10);
    const validTemp = temp === null || temp === undefined ? null : parseFloat(temp);
    const validHum = hum === null || hum === undefined ? null : parseFloat(hum);
    const validOutsideCampus = Boolean(outside_campus);
    
    if (isNaN(validLat) || isNaN(validLng) || isNaN(validVibration)) {
      console.warn('[数据上报] 数据格式错误', { 
        id, 
        lat, 
        lng, 
        vibration, 
        clientIp: req.ip 
      });
      return res.status(400).json({ 
        code: -1, 
        msg: '经纬度/振动值必须为有效数字' 
      });
    }

    const adjusted = applyGpsOffset(validLat, validLng, validGpsValid);
    
    // 计算车辆状态
    let status = 'idle';
    if (validSpeed > 0) {
      // 只要有速度，无论是否有震动，都显示为使用中
      status = 'in_use';
    } else if (validVibration > 0 && validSpeed === 0) {
      // 有震动但没有速度，显示为异常
      status = 'abnormal';
    } else {
      // 没有速度也没有震动，显示为空闲
      status = 'idle';
    }
    
    // 更新设备状态（增加GPS有效性标识）
    deviceStatus[id] = {
      lat: adjusted.lat,
      lng: adjusted.lng,
      wifi_lat: Number.isFinite(validWifiLat) ? validWifiLat : null,
      wifi_lng: Number.isFinite(validWifiLng) ? validWifiLng : null,
      wifi_valid: validWifiValid,
      wifi_age_ms: Number.isFinite(validWifiAge) ? validWifiAge : null,
      // 蓝牙定位数据
      ble_lat: Number.isFinite(validBleLat) ? validBleLat : null,
      ble_lng: Number.isFinite(validBleLng) ? validBleLng : null,
      ble_valid: validBleValid,
      ble_age_ms: Number.isFinite(validBleAge) ? validBleAge : null,
      vibration: validVibration,
      speed: validSpeed.toFixed(1),
      direction: direction,
      satellite: validSatellite,
      gps_valid: validGpsValid,
      outside_campus: validOutsideCampus,
      temp: Number.isFinite(validTemp) ? validTemp : null,
      hum: Number.isFinite(validHum) ? validHum : null,
      status: status,
      lastOnline: Date.now(),
      remoteIp: req.ip || req.connection.remoteAddress, // 记录公网客户端IP
      protocol: req.protocol // 记录请求协议（http/https）
    };

    // 更新轨迹数据（仅坐标有效且GPS有效时）
    if (validLat !== 0 && validLng !== 0 && validGpsValid) {
      const now = new Date();
      const hourKey = now.toISOString().slice(0, 13); // YYYY-MM-DDTHH
      const timestamp = now.getTime(); // 添加时间戳
      // 兼容旧格式：将 {path: []} 转换为 segments
      if (!deviceTracks[id]) {
        deviceTracks[id] = { segments: [], updateTime: now.toISOString() };
      }
      if (Array.isArray(deviceTracks[id].path)) {
        const legacyPath = deviceTracks[id].path;
        deviceTracks[id] = {
          segments: legacyPath.length > 0 ? [{
            hourKey: 'legacy',
            startTime: deviceTracks[id].updateTime || now.toISOString(),
            endTime: now.toISOString(),
            path: legacyPath
          }] : [],
          updateTime: now.toISOString()
        };
      }
      if (!Array.isArray(deviceTracks[id].segments)) {
        deviceTracks[id].segments = [];
      }
      // 获取当前小时段
      let currentSeg = deviceTracks[id].segments[deviceTracks[id].segments.length - 1];
      if (!currentSeg || currentSeg.hourKey !== hourKey) {
        currentSeg = {
          hourKey,
          startTime: now.toISOString(),
          endTime: now.toISOString(),
          path: []
        };
        deviceTracks[id].segments.push(currentSeg);
        // 只保留最近3个小时段
        while (deviceTracks[id].segments.length > MAX_HOURLY_SEGMENTS) {
          deviceTracks[id].segments.shift();
        }
      }
      // 追加点到当前小时段，过滤异常跳点
      const lastPoint = currentSeg.path.length > 0 ? currentSeg.path[currentSeg.path.length - 1] : null;
      const shouldAppend = (() => {
        if (!lastPoint || lastPoint.length < 2) return true;
        const [prevLng, prevLat] = lastPoint.map(Number);
        const toRad = (d) => d * Math.PI / 180;
        const R = 6371000;
        const dLat = toRad(adjusted.lat - prevLat);
        const dLng = toRad(adjusted.lng - prevLng);
        const a = Math.sin(dLat/2) ** 2 + Math.cos(toRad(prevLat)) * Math.cos(toRad(adjusted.lat)) * Math.sin(dLng/2) ** 2;
        const c = 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
        const dist = R * c;
        return dist <= MAX_JUMP_METERS;
      })();
      if (shouldAppend) {
        currentSeg.path.push([adjusted.lng, adjusted.lat, timestamp]); // 添加时间戳
      } else {
        console.warn(`[过滤跳点] 设备${id} 坐标跳变过大(> ${MAX_JUMP_METERS}m)，已忽略该点`);
      }
      currentSeg.endTime = now.toISOString();
      deviceTracks[id].updateTime = now.toISOString();
      CACHE_CONFIG.trackDataChanged = true; // 标记轨迹数据变更
    }

    // 标记设备数据变更
    CACHE_CONFIG.deviceDataChanged = true;
    
    console.log(`[HTTPS上报成功] 设备${id} | 公网IP:${req.ip} | 坐标(${adjusted.lat},${adjusted.lng}) | 振动值:${validVibration} | 速度:${validSpeed}km/h | GPS有效:${validGpsValid} | 蓝牙定位有效:${validBleValid} | 温湿度:${validTemp}°C, ${validHum}%`);
    
    // 检测震动并触发报警
    if (validVibration > 0) {
      console.log(`[震动检测] 设备${id} 检测到震动，触发报警 | 震动值:${validVibration}`);
      // 这里可以添加向ESP32发送控制指令的逻辑
      // 例如通过HTTP请求、MQTT或WebSocket
      
      // 示例：向设备发送报警指令
      // 注意：实际实现需要根据ESP32的通信方式进行调整
      try {
        // 这里可以添加与ESP32的通信代码
        console.log(`[设备控制] 向设备${id}发送震动报警指令`);
      } catch (controlErr) {
        console.error(`[设备控制失败] 向设备${id}发送报警指令失败`, controlErr);
      }
    }
    
    broadcastDeviceStatus(id);

    res.status(200).json({ 
      success: true,
      message: `${PUBLIC_SERVER_PROTOCOL.toUpperCase()}数据接收并推送成功`,
      data: deviceStatus[id],
      serverPort: SERVER_PORT,
      publicUrl: `${PUBLIC_SERVER_PROTOCOL}://${PUBLIC_DOMAIN}`
    });
  } catch (err) {
    console.error('[上报处理失败]', {
      error: err.message,
      stack: err.stack,
      body: req.body,
      clientIp: req.ip
    });
    res.status(500).json({ 
      success: false,
      message: "服务器内部错误：" + err.message,
      timestamp: new Date().toISOString(),
      requestId: Date.now().toString(36)
    });
  }
});

// 获取单个设备数据（HTTPS优化）
app.get('/api/device/:id', (req, res) => {
  try {
    const deviceId = req.params.id;
    const status = deviceStatus[deviceId] || { 
      lat: 0, 
      lng: 0, 
      wifi_lat: null,
      wifi_lng: null,
      wifi_valid: false,
      wifi_age_ms: null,
      ble_lat: null,
      ble_lng: null,
      ble_valid: false,
      ble_age_ms: null,
      vibration: 0,
      speed: "--",
      direction: "--",
      satellite: 0,
      gps_valid: false,
      outside_campus: false,
      lastOnline: 0,
      remoteIp: "",
      protocol: "https"
    };
    
    const now = Date.now();
    const timeDiff = now - status.lastOnline;
    status.isOnline = timeDiff < ONLINE_TIMEOUT;
    status.offlineTime = timeDiff / 1000 + '秒'; // 显示离线时长
    
    console.log(`[HTTPS设备状态查询] ${deviceId} | 公网访问IP: ${req.ip} | 最后上报时间: ${new Date(status.lastOnline)} | 在线状态: ${status.isOnline}`);
    
    res.status(200).json({ 
      code: 0, 
      data: status,
      serverPort: SERVER_PORT,
      publicUrl: `${PUBLIC_SERVER_PROTOCOL}://${PUBLIC_DOMAIN}`
    });
  } catch (err) {
    console.error('[获取设备状态失败]', err.stack);
    res.status(500).json({ code: -1, msg: '服务器内部错误' });
  }
});

// 获取所有设备数据（HTTPS优化）
app.get('/api/devices', (req, res) => {
  try {
    const allDevices = Object.entries(deviceStatus).map(([id, status]) => ({
      deviceId: id,
      ...status,
      isOnline: Date.now() - status.lastOnline < ONLINE_TIMEOUT,
      offlineTime: (Date.now() - status.lastOnline) / 1000 + '秒'
    }));
    res.status(200).json({ 
      code: 0, 
      data: allDevices,
      serverInfo: {
        host: 'localhost',
        port: SERVER_PORT,
        publicDomain: PUBLIC_DOMAIN,
        publicProtocol: PUBLIC_SERVER_PROTOCOL,
        publicPort: PUBLIC_SERVER_PORT,
        clientIp: req.ip,
        clientProtocol: req.protocol
      }
    });
  } catch (err) {
    console.error('[获取所有设备状态失败]', err.stack);
    res.status(500).json({ code: -1, msg: '服务器内部错误' });
  }
});

// ===================== 轨迹管理接口（HTTPS优化） =====================
// 获取设备轨迹
app.get('/api/device/:id/track', (req, res) => {
  try {
    const deviceId = req.params.id;
    let track = deviceTracks[deviceId];
    const nowIso = new Date().toISOString();
    if (!track) {
      track = { segments: [], updateTime: nowIso };
    }
    // 兼容旧格式：如果存在path字段则转换
    if (track && Array.isArray(track.path)) {
      track = {
        segments: track.path.length > 0 ? [{
          hourKey: 'legacy',
          startTime: track.updateTime || nowIso,
          endTime: nowIso,
          path: track.path
        }] : [],
        updateTime: nowIso
      };
      deviceTracks[deviceId] = track;
      writeTrackDataFile(deviceTracks);
    }
    // 返回扁平化的合并路径（便于旧前端使用）和分段
    const mergedPath = (track.segments || []).reduce((acc, seg) => acc.concat(seg.path || []), []);
    res.status(200).json({
      code: 0,
      msg: 'success',
      data: {
        segments: track.segments || [],
        updateTime: track.updateTime || nowIso,
        path: mergedPath
      },
      publicUrl: `${PUBLIC_SERVER_PROTOCOL}://${PUBLIC_DOMAIN}`
    });
  } catch (err) {
    console.error('[获取轨迹失败]', err);
    res.status(500).json({ code: -1, msg: '获取轨迹失败：' + err.message });
  }
});

// 清空设备轨迹
app.delete('/api/device/:id/track', (req, res) => {
  try {
    const deviceId = req.params.id;
    const nowIso = new Date().toISOString();
    
    // 真正删除历史轨迹数据
    if (deviceTracks[deviceId]) {
      deviceTracks[deviceId] = { segments: [], updateTime: nowIso };
    } else {
      deviceTracks[deviceId] = { segments: [], updateTime: nowIso };
    }
    
    writeTrackDataFile(deviceTracks);
    res.status(200).json({
      code: 0,
      msg: '已成功清除设备的历史轨迹数据',
      data: true,
      timestamp: nowIso,
      publicUrl: `${PUBLIC_SERVER_PROTOCOL}://${PUBLIC_DOMAIN}`
    });
  } catch (err) {
    console.error('[清空轨迹失败]', err);
    res.status(500).json({ code: -1, msg: '清空轨迹失败：' + err.message });
  }
});

// 删除单个历史分段（通过 hourKey 或 index）
app.delete('/api/device/:id/track/segment', (req, res) => {
  try {
    const deviceId = req.params.id;
    const hourKey = (req.query.hourKey || req.body?.hourKey || '').trim();
    const indexParam = req.query.index ?? req.body?.index;
    let index = Number.isFinite(Number(indexParam)) ? parseInt(indexParam, 10) : null;

    const track = deviceTracks[deviceId];
    if (!track || !Array.isArray(track.segments) || track.segments.length === 0) {
      return res.status(404).json({ code: -1, msg: '未找到历史分段' });
    }

    let before = track.segments.length;
    if (hourKey) {
      track.segments = track.segments.filter(seg => seg.hourKey !== hourKey);
    } else if (index !== null && index >= 0 && index < track.segments.length) {
      track.segments.splice(index, 1);
    } else {
      return res.status(400).json({ code: -1, msg: '请提供有效的 hourKey 或 index' });
    }

    const after = track.segments.length;
    track.updateTime = new Date().toISOString();
    writeTrackDataFile(deviceTracks);
    return res.status(200).json({
      code: 0,
      msg: '分段已删除',
      removed: before - after,
      segments: track.segments
    });
  } catch (err) {
    console.error('[删除历史分段失败]', err);
    res.status(500).json({ code: -1, msg: '删除历史分段失败：' + err.message });
  }
});

// ===================== WebSocket配置（适配wss协议）====================
let server;
let wss;

if (serverConfig.useHttpsServer) {
  console.log('[SSL] 加载自签名证书...');
  const httpsOptions = {
    key: fs.readFileSync(path.join(__dirname, serverConfig.SSL_CERT.keyFile)),
    cert: fs.readFileSync(path.join(__dirname, serverConfig.SSL_CERT.certFile))
  };
  server = https.createServer(httpsOptions, app);
  console.log('[SSL] ✓ 自签名证书加载成功');
} else {
  server = http.createServer(app);
}

wss = new WebSocket.Server({ 
  server,
  clientTracking: true,
  maxPayload: 1024 * 50, // 增大WebSocket载荷限制
  perMessageDeflate: {
    zlibDeflateOptions: {
      chunkSize: 1024,
      memLevel: 7,
      level: 3
    },
    zlibInflateOptions: {
      chunkSize: 10 * 1024
    },
    clientNoContextTakeover: true,
    serverNoContextTakeover: true,
    serverMaxWindowBits: 10,
    concurrencyLimit: 10,
    threshold: 1024
  }
});

// WebSocket客户端集合
const wsClients = new Map(); // 存储WebSocket连接和心跳信息

// WebSocket心跳配置
const HEARTBEAT_CONFIG = {
  interval: 30000, // 心跳间隔（30秒）
  timeout: 60000,  // 心跳超时（60秒）
};

// WebSocket连接处理
wss.on('connection', (ws, req) => {
  const clientIp = req.socket.remoteAddress;
  const clientId = `${clientIp}_${Date.now()}`;
  const protocolDisplay = serverConfig.useHttpsServer ? 'wss' : 'ws';
  
  console.log(`[${protocolDisplay}连接] 客户端IP: ${clientIp}, 客户端ID: ${clientId}`);
  
  // 存储客户端信息
  wsClients.set(clientId, {
    ws,
    clientIp,
    lastHeartbeat: Date.now(),
    heartbeatTimer: null
  });

  // HTTPS场景：增加连接超时设置（ws对象本身没有setTimeout，需设置底层socket）
  if (ws && ws._socket && typeof ws._socket.setTimeout === 'function') {
    ws._socket.setTimeout(300000); // 5分钟超时
    ws._socket.on('timeout', () => {
      try {
        console.log(`[wss超时] 客户端IP: ${clientIp}`);
        ws.close(1008, '连接超时');
      } catch (e) {}
    });
  }

  // 初始化消息：推送所有设备状态 + 配置 + HTTPS服务器信息
  const config = readConfigFile();
  const initMessage = JSON.stringify({
    type: 'init_data',
    devices: config.devices,
    deviceData: Object.entries(deviceStatus).map(([id, status]) => ({
      deviceId: id,
      ...status,
      isOnline: Date.now() - status.lastOnline < ONLINE_TIMEOUT
    })),
    trackData: deviceTracks,
    serverInfo: {
      host: 'localhost',
      port: SERVER_PORT,
      publicDomain: PUBLIC_DOMAIN,
      publicProtocol: PUBLIC_SERVER_PROTOCOL,
      publicPort: PUBLIC_SERVER_PORT,
      onlineTimeout: ONLINE_TIMEOUT / 1000,
      wsUrl: `${WS_PROTOCOL}://${PUBLIC_DOMAIN}`
    }
  });
  
  // HTTPS场景：异步发送初始化消息，避免阻塞
  setTimeout(() => {
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(initMessage);
    }
  }, 100);

  // 启动心跳
  startHeartbeat(clientId);

  // 监听客户端消息（HTTPS优化：增加错误处理）
  ws.on('message', (data) => {
    try {
      const msg = JSON.parse(data.toString());
      console.log('[收到前端wss消息] 客户端IP:', clientIp, '消息:', msg);
      
      // 处理心跳响应
      if (msg.type === 'heartbeat') {
        const clientInfo = wsClients.get(clientId);
        if (clientInfo) {
          clientInfo.lastHeartbeat = Date.now();
        }
        return;
      }
      
      // 处理前端请求
      switch (msg.type) {
        case 'get_device_data':
          // 前端请求单个设备数据
          const deviceData = deviceStatus[msg.deviceId] || {
            lat: 0, lng: 0, vibration: 0, speed: "--", direction: "--", satellite: 0, gps_valid: false, lastOnline: 0
          };
          deviceData.isOnline = Date.now() - deviceData.lastOnline < ONLINE_TIMEOUT;
          ws.send(JSON.stringify({
            type: 'device_data',
            deviceId: msg.deviceId,
            data: deviceData
          }));
          break;
        case 'get_track':
          // 前端请求轨迹数据
          const track = deviceTracks[msg.deviceId] || { path: [], updateTime: new Date().toISOString() };
          ws.send(JSON.stringify({
            type: 'track_data',
            deviceId: msg.deviceId,
            data: track
          }));
          break;
        case 'get_all_devices':
          // 前端请求所有设备数据
          ws.send(JSON.stringify({
            type: 'all_devices',
            data: Object.entries(deviceStatus).map(([id, status]) => ({
              deviceId: id,
              ...status,
              isOnline: Date.now() - status.lastOnline < ONLINE_TIMEOUT
            }))
          }));
          break;
        default:
          ws.send(JSON.stringify({ type: 'error', msg: '未知消息类型' }));
      }
    } catch (err) {
      console.error('[wss消息解析失败] 客户端IP:', clientIp, '错误:', err);
      ws.send(JSON.stringify({ type: 'error', msg: '消息格式必须为JSON' }));
    }
  });

  // 监听断开连接
  ws.on('close', (code, reason) => {
    console.log(`[wss断开] 客户端IP: ${clientIp}, 原因: ${reason.toString()}, 状态码: ${code}`);
    stopHeartbeat(clientId);
    wsClients.delete(clientId);
  });

  // 监听错误
  ws.on('error', (err) => {
    console.error('[wss错误] 客户端IP:', clientIp, '错误:', err);
    stopHeartbeat(clientId);
    wsClients.delete(clientId);
  });

  // ws库不保证存在'timeout'事件，超时在socket层处理（见上）
});

// 启动心跳
function startHeartbeat(clientId) {
  const clientInfo = wsClients.get(clientId);
  if (!clientInfo) return;
  
  clientInfo.heartbeatTimer = setInterval(() => {
    const now = Date.now();
    
    // 检查心跳超时
    if (now - clientInfo.lastHeartbeat > HEARTBEAT_CONFIG.timeout) {
      console.log(`[wss心跳超时] 客户端ID: ${clientId}`);
      try {
        clientInfo.ws.close(1011, '心跳超时');
      } catch (e) {}
      stopHeartbeat(clientId);
      wsClients.delete(clientId);
      return;
    }
    
    // 发送心跳
    if (clientInfo.ws.readyState === WebSocket.OPEN) {
      try {
        clientInfo.ws.send(JSON.stringify({ type: 'heartbeat' }));
      } catch (e) {
        console.error('[wss心跳发送失败] 客户端ID:', clientId, '错误:', e);
        stopHeartbeat(clientId);
        wsClients.delete(clientId);
      }
    }
  }, HEARTBEAT_CONFIG.interval);
}

// 停止心跳
function stopHeartbeat(clientId) {
  const clientInfo = wsClients.get(clientId);
  if (clientInfo && clientInfo.heartbeatTimer) {
    clearInterval(clientInfo.heartbeatTimer);
  }
}

// 广播设备状态更新（HTTPS优化：增加重试）
function broadcastDeviceStatus(deviceId) {
  const status = deviceStatus[deviceId];
  if (!status) return;

  const message = JSON.stringify({
    type: 'device_update',
    deviceId,
    data: {
      ...status,
      isOnline: true
    }
  });

  if (wsClients.size === 0) {
    console.warn(`[wss广播警告] 没有活跃的WebSocket客户端，跳过广播（设备${deviceId}）`);
    return;
  }

  wsClients.forEach((clientInfo, clientId) => {
    try {
      if (clientInfo.ws.readyState === WebSocket.OPEN) {
        // HTTPS场景：使用异步发送，避免阻塞
        clientInfo.ws.send(message, (err) => {
          if (err) {
            console.error(`[wss广播失败] 设备${deviceId}, 客户端${clientId}`, err);
            // 尝试清理无效连接
            try {
              clientInfo.ws.close(1011, '发送失败');
            } catch (e) {}
            stopHeartbeat(clientId);
            wsClients.delete(clientId);
          }
        });
      } else {
        // 清理无效连接
        console.warn(`[wss广播警告] 客户端${clientId}连接状态无效，清理连接`);
        stopHeartbeat(clientId);
        wsClients.delete(clientId);
      }
    } catch (err) {
      console.error(`[wss广播失败] 设备${deviceId}, 客户端${clientId}`, err);
      stopHeartbeat(clientId);
      wsClients.delete(clientId);
    }
  });
}

// ===================== 定时任务（HTTPS优化）====================
// 定时清理离线超久的设备数据
setInterval(() => {
  const OFFLINE_CLEAN_TIMEOUT = 60 * 60 * 1000; // HTTPS场景延长到1小时
  const now = Date.now();
  let cleanedCount = 0;

  for (const id in deviceStatus) {
    if (now - deviceStatus[id].lastOnline > OFFLINE_CLEAN_TIMEOUT) {
      delete deviceStatus[id];
      cleanedCount++;
    }
  }

  if (cleanedCount > 0) {
    console.log(`[定时清理] 移除${cleanedCount}个离线超1小时的设备状态`);
    writeDeviceDataFile(deviceStatus); // 保存清理后的数据
  }
}, CLEAN_INTERVAL);

// 批量写入数据函数
function batchWriteData() {
  const now = Date.now();
  const shouldWrite = 
    (CACHE_CONFIG.deviceDataChanged || CACHE_CONFIG.trackDataChanged) && 
    (now - CACHE_CONFIG.lastWriteTime >= CACHE_CONFIG.writeInterval);
  
  if (shouldWrite) {
    let deviceWriteSuccess = true;
    let trackWriteSuccess = true;
    
    if (CACHE_CONFIG.deviceDataChanged) {
      deviceWriteSuccess = writeDeviceDataFile(deviceStatus);
      if (deviceWriteSuccess) {
        CACHE_CONFIG.deviceDataChanged = false;
      } else {
        console.error('[批量保存] 设备数据写入失败，将在下次重试');
      }
    }
    if (CACHE_CONFIG.trackDataChanged) {
      trackWriteSuccess = writeTrackDataFile(deviceTracks);
      if (trackWriteSuccess) {
        CACHE_CONFIG.trackDataChanged = false;
      } else {
        console.error('[批量保存] 轨迹数据写入失败，将在下次重试');
      }
    }
    
    if (deviceWriteSuccess || trackWriteSuccess) {
      CACHE_CONFIG.lastWriteTime = now;
      console.log(`[批量保存] 数据已批量保存`);
    }
  }
}

// 批量写入定时任务
setInterval(batchWriteData, 1000); // 每秒检查一次

// 定时保存轨迹数据（作为备份）
setInterval(() => {
  writeTrackDataFile(deviceTracks);
  console.log(`[定时保存] 轨迹数据已保存，当前轨迹数: ${Object.keys(deviceTracks).length}`);
}, 5 * 60 * 1000); // 每5分钟保存一次

// ===================== 优雅关闭（HTTPS优化）====================
process.on('SIGINT', () => {
  console.log('\n[服务器关闭] 开始清理资源（HTTPS环境）...');
  
  // 保存数据
  writeDeviceDataFile(deviceStatus);
  writeTrackDataFile(deviceTracks);
  console.log('[数据保存] 设备状态和轨迹已持久化');
  
  // 关闭WebSocket
  wss.close((err) => {
    if (err) {
      console.error('[wss关闭错误]', err);
    } else {
      console.log('[wss] 已关闭所有连接');
    }
  });
  
  // 关闭HTTP服务器
  server.close((err) => {
    if (err) {
      console.error('[HTTP服务器关闭错误]', err);
      process.exit(1);
    } else {
      console.log('[HTTP服务器] 已停止监听端口', SERVER_PORT);
      process.exit(0);
    }
  });

  // 超时强制退出
  setTimeout(() => {
    console.error('[服务器关闭超时] 强制退出');
    process.exit(1);
  }, 10000);
});

// 捕获未处理的异常
process.on('uncaughtException', (err) => {
  console.error('[未捕获异常]', err);
  // 保存数据后退出
  writeDeviceDataFile(deviceStatus);
  writeTrackDataFile(deviceTracks);
  process.exit(1);
});

process.on('unhandledRejection', (reason) => {
  console.error('[未处理的Promise拒绝]', reason);
});

// ===================== 启动服务器（适配ngrok HTTPS）====================
initData(); // 初始化数据

function startServer() {
  const modeDisplay = serverConfig.MODE.toUpperCase();
  const publicUrl = `${PUBLIC_SERVER_PROTOCOL}://${PUBLIC_DOMAIN}`;
  const localProtocol = serverConfig.useHttpsServer ? 'https' : 'http';
  
  server.listen(SERVER_PORT, () => {
    printServerInfo(modeDisplay, publicUrl, localProtocol);
  });
  
  server.on('error', (err) => {
    console.error('[服务器启动失败]', err);
    process.exit(1);
  });
}

function getLocalIPs() {
  const os = require('os');
  const interfaces = os.networkInterfaces();
  const ips = [];
  
  for (const name of Object.keys(interfaces)) {
    for (const iface of interfaces[name]) {
      if (iface.family === 'IPv4' && !iface.internal) {
        ips.push(iface.address);
      }
    }
  }
  return ips;
}

function printServerInfo(modeDisplay, publicUrl, localProtocol) {
  const localIPs = getLocalIPs();
  
  console.log(`\n========================================`);
  console.log(`🚀 服务器启动成功！`);
  console.log(`========================================`);
  console.log(`📋 当前模式: ${modeDisplay}`);
  if (serverConfig.useHttpsServer) {
    console.log(`🔐 SSL证书: 已启用（自签名证书）`);
  }
  console.log(`📍 本机访问: ${localProtocol}://127.0.0.1:${SERVER_PORT}`);
  
  if (localIPs.length > 0) {
    console.log(`📱 局域网访问:`);
    localIPs.forEach(ip => {
      console.log(`   → ${localProtocol}://${ip}:${SERVER_PORT}`);
    });
  }
  
  console.log(`🌐 公网访问: ${publicUrl}`);
  console.log(`📡 WebSocket: ${WS_PROTOCOL}://127.0.0.1:${SERVER_PORT}`);
  console.log(`📊 监控页面: ${localProtocol}://127.0.0.1:${SERVER_PORT}/gps-monitor.html`);
  console.log(`📤 ESP32上报: ${publicUrl}/api/upload`);
  console.log(`⏱  定时清理: 每${CLEAN_INTERVAL/1000/60}分钟`);
  console.log(`⌛ 离线判定: ${ONLINE_TIMEOUT/1000}秒`);
  console.log(`🗺️  轨迹分段: 最多保留${MAX_HOURLY_SEGMENTS}小时段`);
  console.log(`========================================\n`);
  
  if (serverConfig.useHttpsServer) {
    console.log(`⚠️  提示1: 使用自签名证书，浏览器可能会显示安全警告，请继续访问`);
    console.log(`⚠️  提示2: 手机访问请使用上面显示的局域网IP地址`);
  }
}

startServer();

// 导出服务器实例（方便扩展）
module.exports = { app, server, wss };
