# Python Socket 到 ZeroMQ 通信改造提示词

## 背景信息

项目原来采用 Lua 原生 socket 通信方案，已改造成 C++ 和 ZeroMQ 通信。现需要将 Python 服务端的 socket 通信改造成 ZeroMQ 通信，与现有 C++ 代码兼容。

### 现有 C++ 实现参考

- **文件位置**: `plugins/aipara/src/common/tcp_socket_sync.h` 和 `plugins/aipara/src/common/tcp_socket_sync.cc`
- **通信模式**:
  - **Rime 状态服务** (端口 10086): 使用 ZeroMQ `REQ/REP` 模式
    - C++ 端: `zmq::socket_type::req` (客户端)
    - Python 端: 应改为 `zmq.REP` (服务端)
  - **AI 转换服务** (端口 10087): 使用 ZeroMQ `DEALER/ROUTER` 模式
    - C++ 端: `zmq::socket_type::dealer` (客户端)
    - Python 端: 应改为 `zmq.ROUTER` (服务端)

### 关键配置参数

```cpp
constexpr int kDefaultRimeTimeoutMs = 100;      // Rime 端超时: 100ms
constexpr int kDefaultAiTimeoutMs = 5000;       // AI 端超时: 5000ms
constexpr int kDefaultHwm = 100;                // 高水位线: 100
```

---

## 改造指南

### 1. 依赖库替换

**原来** (Python socket):
```python
import socket
import json
import threading
```

**改为** (ZeroMQ):
```python
import zmq
import json
import threading
```

### 2. 服务端初始化

#### Rime 状态服务 (REP 模式)

**原来**:
```python
self.rime状态接口 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
self.rime状态接口.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
self.rime状态接口.bind((self.host, self.rime_state_port))
self.rime状态接口.listen(10)
```

**改为**:
```python
context = zmq.Context()
self.rime状态接口 = context.socket(zmq.REP)
self.rime状态接口.setsockopt(zmq.LINGER, 0)
self.rime状态接口.setsockopt(zmq.RCVTIMEO, 100)  # 100ms 超时
self.rime状态接口.setsockopt(zmq.SNDTIMEO, 100)
self.rime状态接口.setsockopt(zmq.SNDHWM, 100)
self.rime状态接口.setsockopt(zmq.RCVHWM, 100)
self.rime状态接口.bind(f"tcp://{self.host}:{self.rime_state_port}")
```

#### AI 转换服务 (ROUTER 模式)

**原来**:
```python
self.ai转换接口 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
self.ai转换接口.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
self.ai转换接口.bind((self.host, self.ai_convert_port))
self.ai转换接口.listen(10)
```

**改为**:
```python
context = zmq.Context()
self.ai转换接口 = context.socket(zmq.ROUTER)
self.ai转换接口.setsockopt(zmq.LINGER, 0)
self.ai转换接口.setsockopt(zmq.RCVTIMEO, 5000)  # 5000ms 超时
self.ai转换接口.setsockopt(zmq.SNDTIMEO, 5000)
self.ai转换接口.setsockopt(zmq.SNDHWM, 100)
self.ai转换接口.setsockopt(zmq.RCVHWM, 100)
self.ai转换接口.bind(f"tcp://{self.host}:{self.ai_convert_port}")
```

### 3. 消息接收改造

#### REP 模式 (Rime 状态服务)

**原来**:
```python
def _处理Rime客户端(self, client_socket, client_address, client_id):
    while self.running:
        try:
            data = client_socket.recv(4096).decode("utf-8")
            if not data:
                break
            # 处理数据
            self._处理Rime消息(client_socket, client_id, data)
        except socket.timeout:
            continue
```

**改为**:
```python
def _处理Rime客户端(self):
    while self.running:
        try:
            # ZeroMQ REP 会自动处理客户端身份，直接接收消息
            message = self.rime状态接口.recv_string(zmq.NOBLOCK)
            if not message:
                continue
            # 处理数据
            self._处理Rime消息(message)
            # REP 模式必须发送回复
            self.rime状态接口.send_string(json.dumps(response))
        except zmq.Again:
            # NOBLOCK 模式下无数据时抛出 Again 异常
            time.sleep(0.01)
        except zmq.error.ContextTerminated:
            break
```

#### ROUTER 模式 (AI 转换服务)

**原来**:
```python
def _处理AI客户端(self, client_socket, client_address, client_id):
    while self.running:
        try:
            data = client_socket.recv(4096).decode("utf-8")
            if not data:
                break
            self._处理AI消息(client_socket, client_id, data)
        except socket.timeout:
            continue
```

**改为**:
```python
def _处理AI客户端(self):
    while self.running:
        try:
            # ROUTER 模式会返回 [identity, message]
            frames = self.ai转换接口.recv_multipart(zmq.NOBLOCK)
            if len(frames) < 2:
                continue
            identity = frames[0]  # 客户端身份
            message = frames[1].decode("utf-8")
            
            # 处理数据
            self._处理AI消息(message)
            
            # ROUTER 模式发送时需要加上 identity
            self.ai转换接口.send_multipart([identity, response.encode("utf-8")])
        except zmq.Again:
            time.sleep(0.01)
        except zmq.error.ContextTerminated:
            break
```

### 4. 消息发送改造

#### REP 模式响应

**原来**:
```python
response = json.dumps(response_data) + "\n"
client_socket.send(response.encode("utf-8"))
```

**改为**:
```python
response = json.dumps(response_data, ensure_ascii=False)
self.rime状态接口.send_string(response)
```

#### ROUTER 模式响应

**原来**:
```python
response = json.dumps(response_data) + "\n"
client_socket.send(response.encode("utf-8"))
```

**改为**:
```python
response = json.dumps(response_data, ensure_ascii=False)
# 必须包含客户端 identity
self.ai转换接口.send_multipart([client_identity, response.encode("utf-8")])
```

### 5. 线程模型改造

**原来** (为每个客户端创建线程):
```python
def _接受Rime连接(self):
    while self.running:
        client_socket, client_address = self.rime状态接口.accept()
        client_thread = threading.Thread(
            target=self._处理Rime客户端,
            args=(client_socket, client_address, client_id)
        )
        client_thread.start()
```

**改为** (单线程轮询):
```python
def _处理Rime客户端(self):
    # 不再需要 accept 循环，ZeroMQ 自动处理多客户端
    while self.running:
        try:
            message = self.rime状态接口.recv_string(zmq.NOBLOCK)
            # 处理消息
            response = self._处理Rime消息(message)
            self.rime状态接口.send_string(response)
        except zmq.Again:
            time.sleep(0.01)
```

### 6. 错误处理改造

**原来**:
```python
except socket.timeout:
    continue
except Exception as e:
    logger.error(f"连接失败: {e}")
```

**改为**:
```python
except zmq.Again:
    # 非阻塞模式下无数据
    time.sleep(0.01)
except zmq.error.ContextTerminated:
    # 上下文已关闭
    break
except zmq.error.ZMQError as e:
    logger.error(f"ZeroMQ 错误: {e}")
except Exception as e:
    logger.error(f"处理失败: {e}")
```

### 7. 关闭流程改造

**原来**:
```python
def 停止服务(self):
    self.running = False
    if self.rime状态接口:
        self.rime状态接口.close()
    if self.ai转换接口:
        self.ai转换接口.close()
```

**改为**:
```python
def 停止服务(self):
    self.running = False
    if self.rime状态接口:
        self.rime状态接口.close()
    if self.ai转换接口:
        self.ai转换接口.close()
    if self.context:
        self.context.term()  # 关闭 ZeroMQ 上下文
```

---

## 关键差异总结

| 特性 | Socket | ZeroMQ |
|------|--------|--------|
| **连接模式** | 一对一 TCP | 多对多消息队列 |
| **Rime 服务** | `accept()` 循环 | REP 自动处理 |
| **AI 服务** | `accept()` 循环 | ROUTER 自动处理 |
| **超时处理** | `socket.timeout` | `zmq.Again` |
| **多帧消息** | 手动拼接 | `recv_multipart()` |
| **客户端身份** | 地址元组 | ROUTER 的 identity 帧 |
| **线程模型** | 每客户端一线程 | 单线程轮询 |
| **关闭** | `close()` | `close()` + `term()` |

---

## 流式传输改造

### 原来的流式处理

```python
def _处理流式云转换响应(self, client_socket, ...):
    # 多次发送流式数据
    for chunk in stream:
        response = {...}
        client_socket.send(json.dumps(response).encode("utf-8"))
```

### 改为 ZeroMQ

```python
def _处理流式云转换响应(self, ...):
    # ROUTER 模式下需要保存 identity
    while True:
        try:
            frames = self.ai转换接口.recv_multipart(zmq.NOBLOCK)
            if len(frames) >= 2:
                client_identity = frames[0]
                # 处理消息...
                # 发送流式响应
                for chunk in stream:
                    response = {...}
                    self.ai转换接口.send_multipart([
                        client_identity,
                        json.dumps(response).encode("utf-8")
                    ])
        except zmq.Again:
            time.sleep(0.01)
```

---

## 测试检查清单

- [ ] Rime 状态服务能正常接收 C++ 客户端的 REQ 请求
- [ ] Rime 状态服务能正常发送 REP 响应
- [ ] AI 转换服务能正常接收 C++ 客户端的 DEALER 消息
- [ ] AI 转换服务能正常发送 ROUTER 响应
- [ ] 流式消息能正确分帧发送
- [ ] 超时处理正确（100ms for Rime, 5000ms for AI）
- [ ] 多客户端并发连接正常工作
- [ ] 服务关闭时无资源泄漏

---

## 参考资源

- **ZeroMQ Python 文档**: http://zguide.zeromq.org/py:all
- **REQ/REP 模式**: 同步请求-应答，适合 Rime 状态查询
- **ROUTER/DEALER 模式**: 异步消息队列，适合 AI 流式转换
- **多帧消息**: ZeroMQ 支持原生多帧，无需手动拼接
