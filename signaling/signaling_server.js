/**
 * WebRTC信令服务器 - 基于Node.js和WebSocket (修改版)
 *
 * 此服务器支持以下功能：
 * 1. 客户端注册和房间管理
 * 2. SDP Offer/Answer/ICE Candidate 的一对一转发
 * 3. 连接状态管理
 *
 * 使用方法：
 * 1. 安装依赖: npm install ws
 * 2. 运行服务器: node signaling_server.js [port]
 * 
 */

const WebSocket = require('ws');
const http = require('http');

// 默认端口
const PORT = process.argv[2] || 8080;

// 创建HTTP服务器
const server = http.createServer((req, res) => {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('WebRTC Signaling Server\n');
});

// 创建WebSocket服务器
const wss = new WebSocket.Server({ server });

// 房间和客户端管理
const rooms = new Map(); // 房间ID -> Set<WebSocket>
const clients = new Map(); // WebSocket -> ClientInfo

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

// 转发消息给指定客户端
function forwardTo(fromWs, message) {
    const fromClientInfo = clients.get(fromWs);
    if (!fromClientInfo) {
        sendError(fromWs, 'Cannot forward message: sender is not registered.');
        return;
    }

    const { roomId, clientId: fromClientId } = fromClientInfo;
    const toClientId = message.to;

    // **【修改点】确保所有需要转发的消息都有目标ID**
    if (!toClientId) {
        sendError(fromWs, `Target client ID ("to") is required for messages of type "${message.type}".`);
        return;
    }

    const room = rooms.get(roomId);
    if (!room) {
        sendError(fromWs, `Room ${roomId} not found.`);
        return;
    }

    let found = false;
    for (const clientWs of room) {
        const toClientInfo = clients.get(clientWs);
        if (toClientInfo && toClientInfo.clientId === toClientId) {
            if (clientWs.readyState === WebSocket.OPEN) {
                // 为转发的消息添加发送方信息
                const forwardedMessage = { ...message, from: fromClientId };
                sendMessage(clientWs, forwardedMessage);
                found = true;
                log(`Forwarded message type "${message.type}" from ${fromClientId} to ${toClientId} in room ${roomId}`);
            }
            break;
        }
    }

    if (!found) {
        sendError(fromWs, `Client "${toClientId}" not found in room "${roomId}".`);
    }
}

// 处理注册请求
function handleRegister(ws, message) {
    const { roomId } = message;
    const clientId = message.clientId || `client_${Date.now()}_${Math.floor(Math.random() * 1000)}`;

    if (!roomId) {
        sendError(ws, 'Room ID is required for registration.');
        return;
    }

    const clientInfo = new ClientInfo(ws, clientId, roomId);
    clients.set(ws, clientInfo);

    if (!rooms.has(roomId)) {
        rooms.set(roomId, new Set());
    }
    const room = rooms.get(roomId);

    // 通知新客户端，房间里已有的其他客户端
    for (const otherWs of room) {
        const otherClientInfo = clients.get(otherWs);
        if(otherClientInfo) {
            sendMessage(ws, {
                type: 'client_exists',
                clientId: otherClientInfo.clientId,
                roomId: roomId
            });
        }
    }

    room.add(ws);
    log(`Client ${clientId} registered in room ${roomId}. Total clients in room: ${room.size}`);

    sendMessage(ws, {
        type: 'register_success', // 使用更明确的类型
        clientId: clientId,
        roomId: roomId,
        message: `Registered successfully in room ${roomId} as ${clientId}`
    });

    // 通知房间中的其他客户端有新人加入
    for (const otherWs of room) {
        if (otherWs !== ws) {
            sendMessage(otherWs, {
                type: 'client_joined',
                clientId: clientId,
                roomId: roomId
            });
        }
    }
}

// 处理离开或断线
function handleLeave(ws) {
    const clientInfo = clients.get(ws);
    if (!clientInfo) {
        return; // 该客户端可能从未成功注册
    }

    const { roomId, clientId } = clientInfo;
    const room = rooms.get(roomId);

    if (room) {
        room.delete(ws);
        log(`Client ${clientId} left room ${roomId}. Remaining clients: ${room.size}`);

        if (room.size === 0) {
            rooms.delete(roomId);
            log(`Room ${roomId} is now empty and has been deleted.`);
        } else {
            // 通知房间里的其他人
            for (const otherWs of room) {
                sendMessage(otherWs, {
                    type: 'client_left',
                    clientId: clientId,
                    roomId: roomId
                });
            }
        }
    }
    clients.delete(ws);
}


// WebSocket连接处理
wss.on('connection', (ws) => {
    log('New client connected.');

    // 处理消息
    ws.on('message', (data) => {
        let message;
        try {
            // **【修改点】增强JSON解析的错误处理**
            message = JSON.parse(data);
        } catch (e) {
            log(`Received invalid JSON from a client. Error: ${e.message}`);
            sendError(ws, 'Invalid JSON format. Please send a valid JSON string.');
            return;
        }

        if (!message.type) {
            sendError(ws, 'Message "type" is required.');
            return;
        }

        // 根据消息类型处理
        switch (message.type) {
            case 'register':
                handleRegister(ws, message);
                break;
            
            // **【修改点】offer, answer, candidate统一由forwardTo处理**
            case 'offer':
            case 'answer':
            case 'candidate':
                forwardTo(ws, message);
                break;

            case 'leave':
                handleLeave(ws);
                break;
                
            default:
                log(`Received unknown message type: ${message.type}`);
                sendError(ws, `Unknown message type: "${message.type}"`);
                break;
        }
    });

    // 处理连接关闭
    ws.on('close', () => {
        log('Client disconnected.');
        handleLeave(ws);
    });

    // 处理错误
    ws.on('error', (error) => {
        log(`WebSocket error: ${error.message}`);
        handleLeave(ws); // 发生错误时也执行离开逻辑
    });
});

// 启动服务器
server.listen(PORT, () => {
    log(`Signaling server running on http://localhost:${PORT}`);
});

// // 处理进程退出
// process.on('SIGINT', () => {
//     log('Shutting down server...');
//     wss.close(() => {
//         server.close(() => {
//             process.exit(0);
//         });
//     });
// });
// [新版本] 更强健的关闭逻辑
process.on('SIGINT', () => {
    log('Shutting down server...');

    // 1. 强制终止所有现有的客户端连接，不再等待它们的回应。
    log(`Forcibly terminating ${wss.clients.size} clients...`);
    for (const client of wss.clients) {
        client.terminate();
    }

    // 2. 直接关闭底层的HTTP服务器。当HTTP服务器关闭后，WebSocket服务器也会随之关闭。
    server.close(() => {
        log('Server has been shut down.');
        process.exit(0); // 3. 成功关闭后，退出进程。
    });

    // 增加一个超时强制退出，防止server.close()也意外卡住
    setTimeout(() => {
        log('Shutdown timeout, forcing exit.');
        process.exit(1);
    }, 2000); // 2秒后如果还没退出，就强制退出
});