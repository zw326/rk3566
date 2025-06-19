/**
 * WebRTC信令服务器 - 基于Node.js和WebSocket
 * 
 * 此服务器支持以下功能：
 * 1. 客户端注册和房间管理
 * 2. SDP Offer/Answer交换
 * 3. ICE候选转发
 * 4. 连接状态管理
 * 
 * 使用方法：
 * 1. 安装依赖: npm install ws
 * 2. 运行服务器: node signaling_server.js [port]
 */

const WebSocket = require('ws');
const http = require('http' );
const url = require('url');

// 默认端口
const PORT = process.argv[2] || 8080;

// 创建HTTP服务器
const server = http.createServer((req, res ) => {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('WebRTC Signaling Server\n');
});

// 创建WebSocket服务器
const wss = new WebSocket.Server({ server });

// 房间和客户端管理
const rooms = new Map(); // 房间ID -> 客户端集合
const clients = new Map(); // WebSocket -> 客户端信息

// 客户端信息结构
class ClientInfo {
    constructor(ws, clientId, roomId) {
        this.ws = ws;
        this.clientId = clientId;
        this.roomId = roomId;
        this.timestamp = Date.now();
    }
}

// 日志函数
function log(message) {
    const timestamp = new Date().toISOString();
    console.log(`[${timestamp}] ${message}`);
}

// 发送消息
function sendMessage(ws, message) {
    if (ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(message));
    }
}

// 发送错误消息
function sendError(ws, message) {
    sendMessage(ws, {
        type: 'error',
        message: message
    });
}

// 处理注册请求
function handleRegister(ws, message) {
    const roomId = message.roomId;
    const clientId = message.clientId || `client_${Date.now()}_${Math.floor(Math.random() * 1000)}`;
    
    if (!roomId) {
        sendError(ws, 'Room ID is required');
        return;
    }
    
    // 创建客户端信息
    const clientInfo = new ClientInfo(ws, clientId, roomId);
    clients.set(ws, clientInfo);
    
    // 创建或获取房间
    if (!rooms.has(roomId)) {
        rooms.set(roomId, new Set());
    }
    const room = rooms.get(roomId);
    room.add(ws);
    
    log(`Client ${clientId} registered in room ${roomId}`);
    
    // 发送确认消息
    sendMessage(ws, {
        type: 'register',
        success: true,
        clientId: clientId,
        roomId: roomId,
        message: `Registered in room ${roomId} as ${clientId}`
    });
    
    // 通知房间中的其他客户端
    for (const client of room) {
        if (client !== ws && client.readyState === WebSocket.OPEN) {
            const otherClientInfo = clients.get(client);
            sendMessage(client, {
                type: 'client_joined',
                clientId: clientId,
                roomId: roomId,
                timestamp: Date.now()
            });
            
            // 通知新客户端有关现有客户端
            sendMessage(ws, {
                type: 'client_exists',
                clientId: otherClientInfo.clientId,
                roomId: roomId,
                timestamp: Date.now()
            });
        }
    }
}

// 处理离开请求
function handleLeave(ws) {
    const clientInfo = clients.get(ws);
    if (!clientInfo) {
        return;
    }
    
    const roomId = clientInfo.roomId;
    const clientId = clientInfo.clientId;
    
    // 从房间中移除客户端
    if (rooms.has(roomId)) {
        const room = rooms.get(roomId);
        room.delete(ws);
        
        // 如果房间为空，删除房间
        if (room.size === 0) {
            rooms.delete(roomId);
            log(`Room ${roomId} deleted (empty)`);
        } else {
            // 通知房间中的其他客户端
            for (const client of room) {
                if (client.readyState === WebSocket.OPEN) {
                    sendMessage(client, {
                        type: 'client_left',
                        clientId: clientId,
                        roomId: roomId,
                        timestamp: Date.now()
                    });
                }
            }
        }
    }
    
    // 移除客户端信息
    clients.delete(ws);
    log(`Client ${clientId} left room ${roomId}`);
}

// 处理Offer
function handleOffer(ws, message) {
    const clientInfo = clients.get(ws);
    if (!clientInfo) {
        sendError(ws, 'Not registered');
        return;
    }
    
    const roomId = clientInfo.roomId;
    const fromClientId = clientInfo.clientId;
    const toClientId = message.to;
    
    if (!rooms.has(roomId)) {
        sendError(ws, `Room ${roomId} not found`);
        return;
    }
    
    const room = rooms.get(roomId);
    
    // 如果指定了目标客户端，只发送给该客户端
    if (toClientId) {
        let found = false;
        for (const client of room) {
            const info = clients.get(client);
            if (info && info.clientId === toClientId && client.readyState === WebSocket.OPEN) {
                sendMessage(client, {
                    type: 'offer',
                    from: fromClientId,
                    to: toClientId,
                    roomId: roomId,
                    sdp: message.sdp,
                    timestamp: Date.now()
                });
                found = true;
                break;
            }
        }
        
        if (!found) {
            sendError(ws, `Client ${toClientId} not found in room ${roomId}`);
        }
    } else {
        // 否则广播给房间中的所有其他客户端
        for (const client of room) {
            if (client !== ws && client.readyState === WebSocket.OPEN) {
                const info = clients.get(client);
                sendMessage(client, {
                    type: 'offer',
                    from: fromClientId,
                    to: info.clientId,
                    roomId: roomId,
                    sdp: message.sdp,
                    timestamp: Date.now()
                });
            }
        }
    }
}

// 处理Answer
function handleAnswer(ws, message) {
    const clientInfo = clients.get(ws);
    if (!clientInfo) {
        sendError(ws, 'Not registered');
        return;
    }
    
    const roomId = clientInfo.roomId;
    const fromClientId = clientInfo.clientId;
    const toClientId = message.to;
    
    if (!toClientId) {
        sendError(ws, 'Target client ID is required');
        return;
    }
    
    if (!rooms.has(roomId)) {
        sendError(ws, `Room ${roomId} not found`);
        return;
    }
    
    const room = rooms.get(roomId);
    
    // 查找目标客户端
    let found = false;
    for (const client of room) {
        const info = clients.get(client);
        if (info && info.clientId === toClientId && client.readyState === WebSocket.OPEN) {
            sendMessage(client, {
                type: 'answer',
                from: fromClientId,
                to: toClientId,
                roomId: roomId,
                sdp: message.sdp,
                timestamp: Date.now()
            });
            found = true;
            break;
        }
    }
    
    if (!found) {
        sendError(ws, `Client ${toClientId} not found in room ${roomId}`);
    }
}

// 处理ICE候选
function handleCandidate(ws, message) {
    const clientInfo = clients.get(ws);
    if (!clientInfo) {
        sendError(ws, 'Not registered');
        return;
    }
    
    const roomId = clientInfo.roomId;
    const fromClientId = clientInfo.clientId;
    const toClientId = message.to;
    
    if (!rooms.has(roomId)) {
        sendError(ws, `Room ${roomId} not found`);
        return;
    }
    
    const room = rooms.get(roomId);
    
    // 如果指定了目标客户端，只发送给该客户端
    if (toClientId) {
        let found = false;
        for (const client of room) {
            const info = clients.get(client);
            if (info && info.clientId === toClientId && client.readyState === WebSocket.OPEN) {
                sendMessage(client, {
                    type: 'candidate',
                    from: fromClientId,
                    to: toClientId,
                    roomId: roomId,
                    candidate: message.candidate,
                    sdpMid: message.sdpMid,
                    sdpMLineIndex: message.sdpMLineIndex,
                    timestamp: Date.now()
                });
                found = true;
                break;
            }
        }
        
        if (!found) {
            sendError(ws, `Client ${toClientId} not found in room ${roomId}`);
        }
    } else {
        // 否则广播给房间中的所有其他客户端
        for (const client of room) {
            if (client !== ws && client.readyState === WebSocket.OPEN) {
                const info = clients.get(client);
                sendMessage(client, {
                    type: 'candidate',
                    from: fromClientId,
                    to: info.clientId,
                    roomId: roomId,
                    candidate: message.candidate,
                    sdpMid: message.sdpMid,
                    sdpMLineIndex: message.sdpMLineIndex,
                    timestamp: Date.now()
                });
            }
        }
    }
}

// 处理错误
function handleError(ws, message) {
    const clientInfo = clients.get(ws);
    if (!clientInfo) {
        return;
    }
    
    log(`Error from client ${clientInfo.clientId}: ${message.message}`);
}

// 处理WebSocket连接
wss.on('connection', (ws) => {
    log('New WebSocket connection');
    
    // 处理消息
    ws.on('message', (data) => {
        try {
            const message = JSON.parse(data);
            
            // 检查消息类型
            if (!message.type) {
                sendError(ws, 'Message type is required');
                return;
            }
            
            // 根据消息类型处理
            switch (message.type) {
                case 'register':
                    handleRegister(ws, message);
                    break;
                case 'offer':
                    handleOffer(ws, message);
                    break;
                case 'answer':
                    handleAnswer(ws, message);
                    break;
                case 'candidate':
                    handleCandidate(ws, message);
                    break;
                case 'leave':
                    handleLeave(ws);
                    break;
                case 'error':
                    handleError(ws, message);
                    break;
                default:
                    log(`Unknown message type: ${message.type}`);
                    break;
            }
        } catch (e) {
            log(`Error processing message: ${e.message}`);
        }
    });
    
    // 处理连接关闭
    ws.on('close', () => {
        handleLeave(ws);
    });
    
    // 处理错误
    ws.on('error', (error) => {
        log(`WebSocket error: ${error.message}`);
        handleLeave(ws);
    });
});

// 启动服务器
server.listen(PORT, () => {
    log(`Signaling server running on port ${PORT}`);
});

// 处理进程退出
process.on('SIGINT', () => {
    log('Shutting down...');
    wss.close(() => {
        server.close(() => {
            process.exit(0);
        });
    });
});
