--[[
双端口TCP套接字实时状态同步系统
使用双端口TCP套接字实现不同类型的双向通信：
1. Rime状态交互服务（端口10086）- 快速状态响应，0.1秒超时
2. AI转换服务（端口10087）- 智能拼音转中文，5秒超时
--]] -- 添加 ARM64 Homebrew 的 Lua 路径和项目lua目录
local function setup_lua_paths()
    -- 添加 ARM64 Homebrew 路径
    package.path = package.path .. ";/opt/homebrew/share/lua/5.4/?.lua;/opt/homebrew/share/lua/5.4/?/init.lua"
    package.cpath = package.cpath .. ";/opt/homebrew/lib/lua/5.4/?.so;/opt/homebrew/lib/lua/5.4/?/core.so"

    -- 添加项目lua目录到搜索路径（使用绝对路径）
    package.path = package.path ..
                       ";/Users/yangxinyi/Library/Rime/lua/?.lua;/Users/yangxinyi/Library/Rime/lua/?/init.lua"
end

setup_lua_paths()

local logger_module = require("logger")
local json = require("json") -- 项目中的json模块

-- 创建当前模块的日志记录器
local logger = logger_module.create("tcp_socket_sync", {
    enabled = true,
    unique_file_log = false, -- 启用日志以便测试
    log_level = "DEBUG"
    -- console_output = true -- 为特定实例启用控制台输出
})

local socket_ok, socket = pcall(require, "socket") -- TCP套接字通信
if not socket_ok then
    logger.error("无法加载 socket 模块")
end

local tcp_socket_sync = {}

-- 存储更新函数的引用
tcp_socket_sync.update_all_modules_config = nil

-- 全局开关状态（仅内存，不落盘）。键为 option 名，值为 boolean。
tcp_socket_sync.global_option_state = {}
tcp_socket_sync.update_global_option_state = false

-- 记录一个全局开关值
function tcp_socket_sync.set_global_option(name, value)
    if type(name) ~= "string" then
        return
    end
    local bool_val = not not value
    if tcp_socket_sync.global_option_state[name] ~= bool_val then
        tcp_socket_sync.global_option_state[name] = bool_val
        logger.debug(string.format("记录全局开关: %s = %s", name, tostring(bool_val)))
    end
end

-- 将已记录的全局开关应用到当前 context，返回应用的数量
function tcp_socket_sync.apply_global_options_to_context(context)
    if not context then
        return 0
    end
    local applied = 0
    for name, val in pairs(tcp_socket_sync.global_option_state) do
        if context:get_option(name) ~= val then
            context:set_option(name, val)
            applied = applied + 1
            logger.debug(string.format("应用全局开关到context: %s = %s", name, tostring(val)))
        end
    end
    return applied
end

-- 设置配置更新处理器（由外部调用）, 可以由调用者传入一个函数handler, 将这个函数绑定到config_update_handler中.
function tcp_socket_sync.set_config_update_handler(config_update_function, property_update_function)
    tcp_socket_sync.update_all_modules_config = config_update_function
    tcp_socket_sync.property_update_function = property_update_function
end

-- 更新配置
function tcp_socket_sync.update_configs(config)
    if tcp_socket_sync.update_all_modules_config then
        tcp_socket_sync.update_all_modules_config(config)
    end
end

-- 更新context属性
function tcp_socket_sync.update_property(property_name, property_value)
    if tcp_socket_sync.property_update_function then
        tcp_socket_sync.property_update_function(property_name, property_value)
    end
end

-- 遍历表字段并根据类型更新配置，仅在值发生变化时写入
local function update_config_field(config, field_path, field_value)
    -- 根据不同类型调用对应的getter与setter
    local value_type = type(field_value)
    if value_type == "boolean" then
        local current = config:get_bool(field_path)
        if current ~= field_value then
            config:set_bool(field_path, field_value)
            logger.debug("表字段更新布尔值: " .. field_path .. " = " .. tostring(field_value))
            return true
        end
    elseif value_type == "number" then
        if field_value == math.floor(field_value) then
            local current = config:get_int(field_path)
            if current ~= field_value then
                config:set_int(field_path, field_value)
                logger.debug("表字段更新整数: " .. field_path .. " = " .. tostring(field_value))
                return true
            end
        else
            local current = config:get_double(field_path)
            if current ~= field_value then
                config:set_double(field_path, field_value)
                logger.debug("表字段更新浮点数: " .. field_path .. " = " .. tostring(field_value))
                return true
            end
        end
    elseif value_type == "string" then
        local current = config:get_string(field_path)
        if current ~= field_value then
            config:set_string(field_path, field_value)
            logger.debug("表字段更新字符串: " .. field_path .. " = " .. tostring(field_value))
            return true
        end
    else
        logger.warn("表字段类型暂不支持自动更新: " .. field_path .. " 类型: " .. value_type)
    end
    return false
end

-- 递归遍历table并更新有变化的配置项
local function update_config_table(config, base_path, value_table)
    -- 逐个字段检查差异，只有变化才写入
    local changed = false
    for key, field_value in pairs(value_table) do
        local child_path = base_path .. "/" .. tostring(key)
        if type(field_value) == "table" then
            if update_config_table(config, child_path, field_value) then
                changed = true
            end
        else
            if update_config_field(config, child_path, field_value) then
                changed = true
            end
        end
    end
    return changed
end

-- 获取当前时间戳（毫秒）
local function get_current_time_ms()
    return os.time() * 1000 + math.floor((os.clock() % 1) * 1000)
end

-- 双端口TCP套接字系统
local socket_system = {
    -- rime环境变量
    env = nil,
    engine = nil,
    context = nil,

    -- 服务器配置
    host = "127.0.0.1",

    -- Rime状态服务（快速响应）
    rime_state = {
        port = 10086,
        client = nil,
        is_connected = false,
        last_connect_attempt = 0,
        connect_retry_interval = 5000, -- 5秒重连间隔
        connection_failures = 0,
        max_connection_failures = 3,
        write_failure_count = 0,
        max_failure_count = 3,
        timeout = 0, -- 快速响应超时时间
        pending_lines = {}, -- 非阻塞检测期间读取到的消息缓冲（完整行）
        partial_line = nil -- 非阻塞读取期间的半行缓存
    },

    -- AI转换服务（长时间等待）
    ai_convert = {
        port = 10087,
        client = nil,
        is_connected = false,
        last_connect_attempt = 0,
        connect_retry_interval = 5000, -- 5秒重连间隔
        connection_failures = 0,
        max_connection_failures = 3,
        write_failure_count = 0,
        max_failure_count = 3,
        timeout = 0, -- AI转换超时时间
        pending_lines = {}, -- 非阻塞检测期间读取到的消息缓冲（完整行）
        partial_line = nil -- 非阻塞读取期间的半行缓存
    },

    -- 系统状态
    is_initialized = false
}

-- 连接到Rime状态服务端（快速响应）
function tcp_socket_sync.connect_to_rime_server()
    local current_time = get_current_time_ms()
    local rime_state = socket_system.rime_state

    -- 如果已连接，先检测连接是否真的可用
    if rime_state.client and rime_state.is_connected then
        -- 使用我们的连接检测函数来验证
        if tcp_socket_sync.check_rime_connection() then
            logger.debug("Rime状态服务连接检测通过，无需重连")
            return true
        else
            logger.debug("Rime状态服务连接检测失败，需要重连")
            -- 连接已断开，先断开再重连
            tcp_socket_sync.disconnect_from_rime_server()
        end
    end

    -- 检查重连间隔（仅在需要新连接时检查）
    if (current_time - rime_state.last_connect_attempt) < rime_state.connect_retry_interval then
        logger.debug("Rime状态服务重连间隔未到，跳过连接尝试")
        return false
    end

    rime_state.last_connect_attempt = current_time

    -- 确保之前的连接已经完全断开
    if rime_state.client then
        logger.debug("发现残留的Rime客户端连接，强制关闭")
        tcp_socket_sync.disconnect_from_rime_server()
    end

    -- 尝试新连接
    logger.debug("尝试连接到Rime状态服务端: " .. socket_system.host .. ":" .. rime_state.port)

    local client, err = socket.connect(socket_system.host, rime_state.port)
    if client then
        rime_state.client = client
        rime_state.is_connected = true
        rime_state.connection_failures = 0

        -- 设置快速响应超时
        client:settimeout(rime_state.timeout)

        logger.debug("Rime状态服务连接建立成功")
        return true
    else
        rime_state.connection_failures = rime_state.connection_failures + 1
        logger.warn("Rime状态服务连接失败: " .. tostring(err) .. " (失败次数: " ..
                        rime_state.connection_failures .. ")")
        return false
    end
end

-- 连接到AI转换服务端（长时间等待）
function tcp_socket_sync.connect_to_ai_server()
    local current_time = get_current_time_ms()
    local ai_convert = socket_system.ai_convert

    -- 如果已连接，先检测连接是否真的可用
    if ai_convert.client and ai_convert.is_connected then
        -- 使用我们的连接检测函数来验证
        if tcp_socket_sync.check_ai_connection() then
            logger.debug("AI转换服务连接检测通过，无需重连")
            return true
        else
            logger.debug("AI转换服务连接检测失败，需要重连")
            -- 连接已断开，先断开再重连
            tcp_socket_sync.disconnect_from_ai_server()
        end
    end

    -- 检查重连间隔（仅在需要新连接时检查）
    if (current_time - ai_convert.last_connect_attempt) < ai_convert.connect_retry_interval then
        logger.debug("AI转换服务重连间隔未到，跳过连接尝试")
        return false
    end

    ai_convert.last_connect_attempt = current_time

    -- 确保之前的连接已经完全断开
    if ai_convert.client then
        logger.debug("发现残留的AI客户端连接，强制关闭")
        tcp_socket_sync.disconnect_from_ai_server()
    end

    -- 尝试新连接
    logger.debug("尝试连接到AI转换服务端: " .. socket_system.host .. ":" .. ai_convert.port)

    local client, err = socket.connect(socket_system.host, ai_convert.port)
    if client then
        ai_convert.client = client
        ai_convert.is_connected = true
        ai_convert.connection_failures = 0

        -- 设置AI转换超时
        client:settimeout(ai_convert.timeout)

        logger.debug("AI转换服务连接建立成功")
        return true
    else
        ai_convert.connection_failures = ai_convert.connection_failures + 1
        logger.warn("AI转换服务连接失败: " .. tostring(err) .. " (失败次数: " ..
                        ai_convert.connection_failures .. ")")
        return false
    end
end

-- 断开Rime状态服务连接
function tcp_socket_sync.disconnect_from_rime_server()
    local rime_state = socket_system.rime_state
    if rime_state.client then
        pcall(function()
            rime_state.client:close()
        end)
        rime_state.client = nil
    end
    rime_state.is_connected = false
    -- 清空缓冲的未处理行，避免跨连接使用旧数据
    if rime_state.pending_lines then
        rime_state.pending_lines = {}
    end
    logger.debug("Rime状态服务连接已断开")
end

-- 断开AI转换服务连接
function tcp_socket_sync.disconnect_from_ai_server()
    local ai_convert = socket_system.ai_convert
    if ai_convert.client then
        pcall(function()
            ai_convert.client:close()
        end)
        ai_convert.client = nil
    end
    ai_convert.is_connected = false
    if ai_convert.pending_lines then
        ai_convert.pending_lines = {}
    end
    ai_convert.partial_line = nil
    logger.debug("AI转换服务连接已断开")
end

-- 断开与所有服务端的连接
function tcp_socket_sync.disconnect_from_server()
    tcp_socket_sync.disconnect_from_rime_server()
    tcp_socket_sync.disconnect_from_ai_server()
    logger.debug("所有TCP连接已断开")
end

-- 检测AI转换服务连接状态
function tcp_socket_sync.check_ai_connection()
    local ai_convert = socket_system.ai_convert
    if not ai_convert.client or not ai_convert.is_connected then
        logger.debug("AI转换服务未连接")
        return false
    end

    -- 非阻塞读取一行进行探活；读到的数据缓冲起来，不吞消息
    local original_timeout = ai_convert.client:gettimeout()
    ai_convert.client:settimeout(0)
    local line, err, partial = ai_convert.client:receive("*l")
    ai_convert.client:settimeout(original_timeout)

    if line then
        if ai_convert.partial_line then
            line = ai_convert.partial_line .. line
            ai_convert.partial_line = nil
        end
        table.insert(ai_convert.pending_lines, line)
        logger.debug("AI连接检测期间捕获到消息，已缓冲: " .. line)
        return true
    end

    if err == nil then
        return true
    elseif err == "timeout" then
        if partial and #partial > 0 then
            ai_convert.partial_line = (ai_convert.partial_line or "") .. partial
            logger.debug("AI连接检测期间捕获到半行数据，已暂存，长度: " ..
                             tostring(#ai_convert.partial_line))
        end
        return true
    elseif err == "closed" then
        logger.warn("检测到AI转换服务连接已断开")
        tcp_socket_sync.disconnect_from_ai_server()
        return false
    else
        logger.warn("AI连接检测出现错误: " .. tostring(err))
        return false
    end
end

-- 检测Rime状态服务连接状态
function tcp_socket_sync.check_rime_connection()
    local rime_state = socket_system.rime_state
    if not rime_state.client or not rime_state.is_connected then
        logger.debug("Rime状态服务未连接")
        return false
    end

    local original_timeout = rime_state.client:gettimeout()
    rime_state.client:settimeout(0) -- 非阻塞
    local line, err, partial = rime_state.client:receive("*l")
    rime_state.client:settimeout(original_timeout)

    if line then
        -- 若之前有半行，拼接后入队（不过 *l 返回的 line 已是不含分隔符的完整行）
        if rime_state.partial_line then
            line = rime_state.partial_line .. line
            rime_state.partial_line = nil
        end
        table.insert(rime_state.pending_lines, line)
        logger.debug("Rime连接检测期间捕获到消息，已缓冲: " .. line)
        return true
    end

    if err == nil then
        -- 无数据，无错误
        return true
    elseif err == "timeout" then
        -- 非阻塞读取可能返回partial（当前行未结束）
        if partial and #partial > 0 then
            rime_state.partial_line = (rime_state.partial_line or "") .. partial
            logger.debug("Rime连接检测期间捕获到半行数据，已暂存，长度: " ..
                             tostring(#rime_state.partial_line))
        end
        return true
    elseif err == "closed" then
        logger.warn("检测到Rime状态服务连接已断开")
        tcp_socket_sync.disconnect_from_rime_server()
        return false
    else
        logger.warn("Rime连接检测出现错误: " .. tostring(err))
        return false
    end
end

-- 写入Rime状态服务TCP套接字
function tcp_socket_sync.write_to_rime_socket(data)
    if not socket_system.is_initialized then
        return false
    end

    local rime_state = socket_system.rime_state

    -- 首先检查连接状态
    if not rime_state.client or not rime_state.is_connected then
        logger.debug("Rime状态服务未连接，尝试连接...")
        if not tcp_socket_sync.connect_to_rime_server() then
            logger.warn("Rime状态服务连接不可用")
            return false
        end
    end

    -- 在发送数据前，先检测连接是否真的可用
    if not tcp_socket_sync.check_rime_connection() then
        logger.warn("Rime连接检测失败，尝试重新连接...")
        -- 尝试重新连接
        if not tcp_socket_sync.connect_to_rime_server() then
            logger.error("Rime状态服务重连失败，放弃数据发送")
            return false
        end

        -- 重连后再次检测
        if not tcp_socket_sync.check_rime_connection() then
            logger.error("Rime状态服务重连后连接检测仍然失败，放弃数据发送")
            return false
        end
    end

    local success, err = pcall(function()
        
        -- 发送JSON数据，以换行符结尾
        -- local original_timeout = rime_state.client:gettimeout()
        -- logger.debug("original_timeout: " .. tostring(original_timeout))
        rime_state.client:send(data .. "\n")
    end)

    if success then
        logger.debug("write_to_rime_socket消息发送成功")
        rime_state.write_failure_count = 0
        return true
    else
        -- send()调用失败，说明连接确实有问题
        rime_state.write_failure_count = rime_state.write_failure_count + 1
        logger.error("Rime状态服务TCP写入失败: " .. tostring(err) .. " (失败次数: " ..
                         rime_state.write_failure_count .. ")")

        -- 连接已断开，立即断开
        tcp_socket_sync.disconnect_from_rime_server()
        return false
    end
end

-- 写入AI转换服务TCP套接字
function tcp_socket_sync.write_to_ai_socket(data)
    if not socket_system.is_initialized then
        return false
    end

    local ai_convert = socket_system.ai_convert

    -- 首先检查连接状态
    if not ai_convert.client or not ai_convert.is_connected then
        logger.debug("AI转换服务未连接，尝试连接...")
        if not tcp_socket_sync.connect_to_ai_server() then
            logger.warn("AI转换服务连接不可用")
            return false
        end
    end

    -- 在发送数据前，先检测连接是否真的可用
    if not tcp_socket_sync.check_ai_connection() then
        logger.warn("AI连接检测失败，尝试重新连接...")
        -- 尝试重新连接
        if not tcp_socket_sync.connect_to_ai_server() then
            logger.error("AI转换服务重连失败，放弃数据发送")
            return false
        end

        -- 重连后再次检测
        if not tcp_socket_sync.check_ai_connection() then
            logger.error("AI转换服务重连后连接检测仍然失败，放弃数据发送")
            return false
        end
    end

    local success, err = pcall(function()
        -- 发送JSON数据，以换行符结尾
        logger.debug("将要发送给客户端的ai接口json:  " .. tostring(data))
        ai_convert.client:send(data .. "\n")
    end)

    if success then
        logger.debug("ai接口数据发送成功")
        ai_convert.write_failure_count = 0
        return true
    else
        -- send()调用失败，说明连接确实有问题
        ai_convert.write_failure_count = ai_convert.write_failure_count + 1
        logger.error("AI转换服务TCP写入失败: " .. tostring(err) .. " (失败次数: " ..
                         ai_convert.write_failure_count .. ")")

        -- 连接已断开，立即断开
        tcp_socket_sync.disconnect_from_ai_server()
        return false
    end
end

-- 非阻塞读取Rime状态服务TCP套接字数据
function tcp_socket_sync.read_from_rime_socket(timeout)
    local rime_state = socket_system.rime_state
    if not rime_state.client or not rime_state.is_connected then
        logger.debug("Rime状态服务未连接，尝试重新连接...")
        if not tcp_socket_sync.connect_to_rime_server() then
            logger.warn("Rime状态服务重连失败")
            return nil
        end
        logger.debug("Rime状态服务重连成功，继续读取数据")
    end

    -- 优先返回检测阶段缓冲的消息，避免消息被测试逻辑吞掉
    if rime_state.pending_lines and #rime_state.pending_lines > 0 then
        local buffered = table.remove(rime_state.pending_lines, 1)
        logger.debug("📥 从缓冲区读取到Rime消息: " .. buffered)
        return buffered
    end

    local line, err, partial
    if timeout then
        local original_timeout = rime_state.client:gettimeout()
        rime_state.client:settimeout(timeout)
        line, err, partial = rime_state.client:receive("*l")
        rime_state.client:settimeout(original_timeout)
    else
        line, err, partial = rime_state.client:receive("*l")
    end

    if line then
        if rime_state.partial_line then
            line = rime_state.partial_line .. line
            rime_state.partial_line = nil
        end
        logger.debug("📥 从Rime状态服务读取到原始数据: " .. line)
        return line
    elseif err == "timeout" then
        -- 保存半行数据以便下次继续拼接
        if partial and #partial > 0 then
            rime_state.partial_line = (rime_state.partial_line or "") .. partial
            logger.debug("⏸️ 收到半行数据，已暂存，当前长度: " .. tostring(#rime_state.partial_line))
        end
        -- 超时表示当前无数据可读，这是正常情况
        return nil
    else
        -- 其他错误，可能是连接断开
        logger.warn("Rime socket服务没有读取到数据: " .. tostring(err))
        -- M.disconnect_from_rime_server()
        return nil
    end
end

-- 带超时读取AI转换服务TCP套接字数据（按行读取，支持自定义超时）
function tcp_socket_sync.read_from_ai_socket(timeout_seconds)
    local ai_convert = socket_system.ai_convert
    if not ai_convert.client or not ai_convert.is_connected then
        logger.debug("AI转换服务未连接，尝试重新连接...")
        if not tcp_socket_sync.connect_to_ai_server() then
            logger.warn("AI转换服务重连失败")
            return nil
        end
        logger.debug("AI转换服务重连成功，继续读取数据")
    end

    -- 设置自定义超时时间
    local original_timeout = ai_convert.timeout
    if timeout_seconds then
        ai_convert.client:settimeout(timeout_seconds)
        logger.debug("🕐 临时设置AI转换服务按行读取超时时间为: " .. timeout_seconds .. "秒")
    end

    -- 优先消费检测阶段缓冲的完整行
    if ai_convert.pending_lines and #ai_convert.pending_lines > 0 then
        local buffered = table.remove(ai_convert.pending_lines, 1)
        if timeout_seconds and ai_convert.client then
            ai_convert.client:settimeout(original_timeout)
            logger.debug("🔄 恢复AI转换服务原始超时时间: " .. original_timeout .. "秒")
        end
        logger.debug("📥 从缓冲区读取到AI消息: " .. buffered)
        return buffered
    end

    local line, err, partial = ai_convert.client:receive("*l")

    -- 恢复原始超时设置
    if timeout_seconds and ai_convert.client then
        ai_convert.client:settimeout(original_timeout)
        logger.debug("🔄 恢复AI转换服务原始超时时间: " .. original_timeout .. "秒")
    end

    if line then
        if ai_convert.partial_line then
            line = ai_convert.partial_line .. line
            ai_convert.partial_line = nil
        end
        logger.debug("📥 从AI转换服务读取到原始数据: " .. line)
        return line
    elseif err == "timeout" then
        -- 超时表示等待时间内无数据可读
        if partial and #partial > 0 then
            ai_convert.partial_line = (ai_convert.partial_line or "") .. partial
            logger.debug("⏸️ 收到半行数据，已暂存，当前长度: " .. tostring(#ai_convert.partial_line))
        end
        logger.warn("⏰ AI转换服务等待超时 (" .. (timeout_seconds or ai_convert.timeout) .. "秒)")
        return nil
    else
        -- 其他错误，可能是连接断开
        logger.warn("AI转换服务TCP读取错误: " .. tostring(err))
        tcp_socket_sync.disconnect_from_ai_server()
        return nil
    end
end

-- 读取AI转换服务TCP套接字所有可用数据（支持自定义超时）
function tcp_socket_sync.read_all_from_ai_socket(timeout_seconds)
    local ai_convert = socket_system.ai_convert
    if not ai_convert.client or not ai_convert.is_connected then
        logger.debug("AI转换服务未连接，尝试重新连接...")
        if not tcp_socket_sync.connect_to_ai_server() then
            logger.warn("AI转换服务重连失败")
            return nil
        end
        logger.debug("AI转换服务重连成功，继续读取数据")
    end

    -- 设置自定义超时时间
    local original_timeout = ai_convert.timeout
    if timeout_seconds then
        ai_convert.client:settimeout(timeout_seconds)
        logger.debug("🕐 临时设置AI转换服务超时时间为: " .. timeout_seconds .. "秒")
    end

    local all_data = ""
    local chunk_size = 8192 -- 每次读取8KB
    local start_time = get_current_time_ms()
    local timeout_ms = (timeout_seconds or ai_convert.timeout) * 1000

    while true do
        -- 检查总体超时时间
        local current_time = get_current_time_ms()
        if (current_time - start_time) > timeout_ms then
            logger.warn("🕐 AI转换服务批量读取总体超时 (" .. (timeout_seconds or ai_convert.timeout) ..
                            "秒)")
            break
        end

        local chunk, err = ai_convert.client:receive(chunk_size)

        if chunk then
            all_data = all_data .. chunk
            logger.debug("📥 从AI转换服务读取到数据块: " .. string.len(chunk) .. " 字节")

            -- 如果读取的数据少于chunk_size，说明没有更多数据了
            if string.len(chunk) < chunk_size then
                break
            end
        elseif err == "timeout" then
            -- 超时表示没有更多数据可读
            if string.len(all_data) > 0 then
                logger.debug("📥 AI转换服务读取完成，总共读取: " .. string.len(all_data) .. " 字节")
            else
                logger.warn("⏰ AI转换服务等待超时，无数据可读 (" ..
                                (timeout_seconds or ai_convert.timeout) .. "秒)")
            end
            break
        else
            -- 其他错误，可能是连接断开
            logger.warn("AI转换服务TCP批量读取错误: " .. tostring(err))
            if string.len(all_data) == 0 then
                tcp_socket_sync.disconnect_from_ai_server()
                -- 恢复原始超时设置
                if timeout_seconds and ai_convert.client then
                    ai_convert.client:settimeout(original_timeout)
                end
                return nil
            end
            break
        end
    end

    -- 恢复原始超时设置
    if timeout_seconds and ai_convert.client then
        ai_convert.client:settimeout(original_timeout)
        logger.debug("🔄 恢复AI转换服务原始超时时间: " .. original_timeout .. "秒")
    end

    if string.len(all_data) > 0 then
        logger.debug("📥 从AI转换服务读取到完整数据: " .. all_data)
        return all_data
    else
        return nil
    end
end

-- 快速清理AI转换服务TCP套接字积压数据
function tcp_socket_sync.flush_ai_socket_buffer()
    local ai_convert = socket_system.ai_convert
    if not ai_convert.client or not ai_convert.is_connected then
        logger.debug("AI转换服务未连接，尝试重新连接...")
        if not tcp_socket_sync.connect_to_ai_server() then
            logger.warn("AI转换服务重连失败，无法清理缓冲区")
            return 0
        end
        logger.debug("AI转换服务重连成功，继续清理缓冲区")
    end

    -- 临时设置为非阻塞模式（0秒超时）
    local original_timeout = ai_convert.timeout
    ai_convert.client:settimeout(0)

    local total_flushed = 0
    local chunk_size = 8192

    -- 快速读取并丢弃所有积压数据
    while true do
        local chunk, err = ai_convert.client:receive(chunk_size)

        if chunk then
            total_flushed = total_flushed + string.len(chunk)
            -- 如果读取的数据少于chunk_size，说明没有更多数据了
            if string.len(chunk) < chunk_size then
                break
            end
        else
            -- 没有更多数据或出错，退出循环
            break
        end
    end

    -- 恢复原始超时设置
    ai_convert.client:settimeout(original_timeout)

    if total_flushed > 0 then
        logger.debug("🗑️ 快速清理AI套接字积压数据: " .. total_flushed .. " 字节")
    end

    return total_flushed
end

-- 读取AI转换服务最新消息（丢弃旧消息，只返回最后一条）- 优化版本
-- 返回值格式: {data = parsed_data or nil, status = "success"|"timeout"|"no_data"|"error", raw_message = string or nil}
function tcp_socket_sync.read_latest_from_ai_socket(timeout_seconds)
    local ai_convert = socket_system.ai_convert
    if not ai_convert.client or not ai_convert.is_connected then
        logger.debug("AI转换服务未连接，尝试重新连接...")
        if not tcp_socket_sync.connect_to_ai_server() then
            logger.warn("AI转换服务重连失败")
            return {
                data = nil,
                status = "error",
                raw_message = nil,
                error_msg = "服务未连接且重连失败"
            }
        end
        logger.debug("AI转换服务重连成功，继续读取数据")
    end

    -- 设置自定义超时时间
    local timeout_seconds = timeout_seconds or 0.1 -- 默认100ms超时

    ai_convert.client:settimeout(timeout_seconds)
    logger.debug("🕐 设置AI转换服务读取超时时间为: " .. timeout_seconds .. "秒")

    -- 使用循环按行读取数据，保留最后一行
    local latest_line = nil
    local total_lines = 0
    local max_attempts = 50 -- 最多尝试50次读取，防止无限循环

    -- 先消费缓冲区里的行
    if ai_convert.pending_lines and #ai_convert.pending_lines > 0 then
        latest_line = table.remove(ai_convert.pending_lines, #ai_convert.pending_lines)
        total_lines = 1
        logger.debug("📥 从缓冲区获取最新AI消息: " .. latest_line)
    else
        -- 无缓冲则尝试从socket拉取
        for attempt = 1, max_attempts do
            local line, err = ai_convert.client:receive("*l")

            if line then
                latest_line = line -- 保存最新的一行
                total_lines = total_lines + 1
                logger.debug("📥 读取到消息行: " .. line)
            elseif err == "timeout" then
                -- 超时表示没有更多数据，退出循环
                logger.debug("⏰ 第 " .. attempt .. " 次读取超时，停止读取")
                break
            else
                -- 其他错误
                logger.warn("AI转换服务TCP读取错误: " .. tostring(err))
                tcp_socket_sync.disconnect_from_ai_server()
                return {
                    data = nil,
                    status = "error",
                    raw_message = nil,
                    error_msg = tostring(err)
                }
            end
        end
    end

    if latest_line then
        if total_lines > 1 then
            logger.debug("🎯 共读取了 " .. total_lines .. " 条消息，丢弃了 " .. (total_lines - 1) ..
                             " 条旧消息，保留最后一条")
        else
            logger.debug("📥 从AI转换服务读取到1条最新消息")
        end

        logger.debug("🎯 返回最新消息: " .. latest_line)

        -- 尝试解析JSON数据
        local parsed_data = tcp_socket_sync.parse_socket_data(latest_line)
        return {
            data = parsed_data,
            status = "success",
            raw_message = latest_line
        }
    else
        -- 没有读取到任何消息
        logger.debug("📭 没有收到有效消息，共尝试了 " .. max_attempts .. " 次读取")
        return {
            data = nil,
            status = "timeout",
            raw_message = nil
        }
    end
end

-- 解析从Python端接收的数据
function tcp_socket_sync.parse_socket_data(data)
    if not data or #data == 0 then
        return nil
    end

    logger.debug("🔍 解析socket数据data: " .. tostring(data) .. " (类型: " .. type(data) .. ")")

    local success, parsed_data = pcall(json.decode, data)

    if success and parsed_data then
        logger.debug("🔍 解析TCP数据成功: " .. tostring(parsed_data))
        return parsed_data
    else
        logger.error("❌ 解析TCP数据失败: " .. tostring(data))
        return nil
    end
end

-- 处理从Python端接收的命令
function tcp_socket_sync.handle_socket_command(command_messege, env)
    -- 从 env 提取 context（可能为nil）
    local context = env.engine.context
    local config = env.engine.schema.config

    --[[ 接收到消息格式: 
    {"messege_type": "command_response", "response": "rime_state_received", "timestamp": 1753022593756, "client_id": "rime-127.0.0.1:57187", "command_messege": [{"command": "set_option", "command_type": "option", "option_name": "full_shape", "option_value": true, "timestamp": 1753022590433}]}
    
    注意：外层的 command_messege 是一个数组，但此函数处理的是数组中的单个命令对象
    ]]

    -- 🎯 处理TCP命令: set_option option_name: super_tips
    logger.debug("🎯 处理TCP命令: " .. command_messege.command)

    local command = command_messege.command
    if command == "ping" then
        -- 响应ping命令
        logger.debug("📞 收到ping命令")
        tcp_socket_sync.write_to_rime_socket('{"response": "pong"}')
        return true
    elseif command == "set_option" then
        -- 修改设置
        logger.debug("command_messege.option_value: " .. tostring(command_messege.option_value))
        if context then
            if context:get_option(command_messege.option_name) ~= command_messege.option_value then
                tcp_socket_sync.update_global_option_state = true
                -- 记录到模块级全局变量，供其他会话/模块读取与应用
                tcp_socket_sync.set_global_option(command_messege.option_name, command_messege.option_value)

            end
            local response = {
                response = "option_set",
                option_name = command_messege.option_name,
                success = true,
                timestamp = get_current_time_ms(),
                responding_to = "set_option"
            }
            tcp_socket_sync.write_to_rime_socket(json.encode(response))
        else
            logger.warn("context为nil，无法设置选项: " .. tostring(command_messege.option_name))
            local response = {
                response = "option_set",
                option_name = command_messege.option_name,
                success = false,
                error = "context is nil",
                timestamp = get_current_time_ms(),
                responding_to = "set_option"
            }
            tcp_socket_sync.write_to_rime_socket(json.encode(response))
        end
        return true
    elseif command == "set_config" then
        -- 配置变更通知
        local config_name = command_messege.config_name
        local config_path = command_messege.config_path
        local config_value = command_messege.config_value
        local description = command_messege.description
        local timestamp = command_messege.timestamp

        logger.info("🔧 收到配置变更通知:")
        logger.info("   配置名称: " .. tostring(config_name))
        logger.info("   配置路径: " .. tostring(config_path))
        logger.info("   配置值: " .. tostring(config_value))
        logger.info("   变更描述: " .. tostring(description))
        logger.info("   时间戳: " .. tostring(timestamp))

        -- 实际更新配置
        
        -- 将点分隔的路径转换为Rime配置路径（用斜杠分隔）
        local rime_config_path = string.gsub(config_path, "%.", "/")
        logger.debug("转换后的配置路径: " .. rime_config_path)

        local success = false
        local need_refresh = false
        if config_value ~= nil then
            local value_type = type(config_value)

            if value_type == "boolean" then
                config:set_bool(rime_config_path, config_value)
                success = true
                need_refresh = true
                logger.debug("设置布尔配置: " .. rime_config_path .. " = " .. tostring(config_value))
            elseif value_type == "number" then
                -- 尝试判断是整数还是浮点数
                if config_value == math.floor(config_value) then
                    config:set_int(rime_config_path, config_value)
                    logger.debug("设置整数配置: " .. rime_config_path .. " = " .. tostring(config_value))
                else
                    config:set_double(rime_config_path, config_value)
                    logger.debug("设置浮点数配置: " .. rime_config_path .. " = " .. tostring(config_value))
                end
                success = true
                need_refresh = true
            elseif value_type == "string" then
                config:set_string(rime_config_path, config_value)
                success = true
                need_refresh = true
                logger.debug("设置字符串配置: " .. rime_config_path .. " = " .. tostring(config_value))
            elseif value_type == "table" then
                -- 表配置按字段比对后再更新
                local changed = update_config_table(config, rime_config_path, config_value)
                success = true
                need_refresh = changed
                if changed then
                    logger.debug("表配置更新完成: " .. rime_config_path)
                else
                    logger.debug("表配置未发生变化: " .. rime_config_path)
                end
            else
                logger.warn("不支持的配置值类型: " .. value_type)
            end

        else
            success = true
            config:set_string(rime_config_path, "__DELETED__")
            need_refresh = true
            logger.debug("设置配置删除标记: " .. rime_config_path .. " = __DELETED__")
            -- logger.warn("配置值为空，跳过更新")
        end
        if success then
            if need_refresh then
                tcp_socket_sync.update_configs(config)
                logger.info("✅ update_all_modules_config配置更新成功")
                -- 更新一个上下文属性
                tcp_socket_sync.update_property("config_update_flag", "1")
            else
                logger.debug("表配置无变化，跳过模块刷新: " .. rime_config_path)
            end
        else
            logger.error("❌ 配置更新失败: " .. rime_config_path)
        end

        return true
    elseif command == "set_property" then
        -- 修改属性
        logger.debug("command_messege.property_name: " .. tostring(command_messege.property_name))
        logger.debug("command_messege.property_value: " .. tostring(command_messege.property_value))
        tcp_socket_sync.update_property(command_messege.property_name, command_messege.property_value)

        return true
    elseif command == "clipboard_data" then
        logger.debug("command_messege: clipboard_data")
        -- 处理获取剪贴板命令：将 clipboard.text 追加到 context.input
        local clipboard = command_messege.clipboard or {}
        local clipboard_text = clipboard.text
        local success_flag = command_messege.success

        if success_flag == false then
            local err_msg = (clipboard and clipboard.error) or command_messege.error or "unknown"
            logger.warn("get_clipboard 返回失败，错误信息: " .. tostring(err_msg))
            return true
        end

        if clipboard_text and clipboard_text ~= "" then
            local english_mode_symbol = config:get_string("translator/english_mode_symbol") or ""
            -- 将英文符号替换成空格.
            if english_mode_symbol ~= "" then
                if clipboard_text:find(english_mode_symbol, 1, true) then
                    clipboard_text = clipboard_text:gsub(
                        english_mode_symbol:gsub("([%^%$%(%)%%%.%[%]%*%+%-%?])", "%%%1"), " ")
                end
            end

            local rawenglish_prompt = context:get_property("rawenglish_prompt")
            if rawenglish_prompt == "1" then
                context.input = context.input .. clipboard_text
                logger.debug("get_clipboard 粘贴clipboard_text: " .. clipboard_text)
            else
                
                context.input = context.input .. english_mode_symbol .. clipboard_text .. english_mode_symbol
                logger.debug("get_clipboard 粘贴clipboard_text: " .. english_mode_symbol .. clipboard_text .. english_mode_symbol)
            end
            
            
        else
            logger.warn("get_clipboard 命令未提供有效的文本可追加")
            -- 在这个地方应该添加一个prompt通知用户, 应该是提取最后一个segment
            local segmentation = context.composition:toSegmentation()
            local last_segment = segmentation:back()
            last_segment.prompt = " [剪贴板为空] "
            
        end

        return true

    elseif command == "paste_executed" then
        -- 粘贴命令执行成功响应
        logger.info("✅ 服务端已成功执行粘贴操作")
        return true
    elseif command == "paste_failed" then
        -- 粘贴命令执行失败响应
        local error_msg = command_messege.error or "未知错误"
        logger.error("❌ 服务端执行粘贴操作失败: " .. tostring(error_msg))
        return true
    else
        logger.warn("❓ 未知的TCP命令: " .. command)
        return false
    end
end

-- 定期处理Rime状态服务TCP套接字数据
function tcp_socket_sync.process_rime_socket_data(env, timeout)
    local processed_any = false
    local processed_count = 0
    local max_messages = 5
    while true do
        if processed_count >= max_messages then
            logger.debug("⏹️ 已达到本次处理上限: " .. tostring(max_messages) .. " 条消息")
            break
        end
        local data = tcp_socket_sync.read_from_rime_socket(timeout)
        if not data then
            break
        end

        logger.debug("🎯 成功接收到Rime状态服务完整消息: " .. data)
        local parsed_data = tcp_socket_sync.parse_socket_data(data)
        if parsed_data then
            logger.debug("📨 Rime状态消息解析成功")
            if parsed_data.messege_type == "command_response" then
                logger.debug("📨 检测到嵌套命令 command_response 字段.")
                -- command_messege 现在是一个数组，可能包含多条命令
                if parsed_data.command_messege then
                    if #parsed_data.command_messege > 0 then
                        -- 如果是数组，遍历处理每个命令
                        for i, command_item in ipairs(parsed_data.command_messege) do
                            logger.debug("📨 处理第 " .. i .. " 条命令: " .. tostring(command_item.command))
                            tcp_socket_sync.handle_socket_command(command_item, env)
                        end
                    else
                        -- 如果是单个命令对象（向后兼容）
                        tcp_socket_sync.handle_socket_command(parsed_data.command_messege, env)
                    end
                end
            elseif parsed_data.messege_type == "command_executed" then
                -- 命令执行成功的通知消息
                logger.info("✅ 收到命令执行成功通知: paste_executed")
                logger.debug("命令执行成功响应内容: " .. data)
            end
            processed_any = true
            processed_count = processed_count + 1
        else
            logger.warn("⚠️  Rime状态消息解析失败")
            -- 解析失败，继续尝试读取下一条
            processed_count = processed_count + 1
        end
    end

    return processed_any -- 处理过至少一条消息则为true，否则false
end

-- 和Rime状态服务进行数据交换
function tcp_socket_sync.sync_with_server(env, option_info, send_commit_text, command_key, command_value, timeout, position, char)
    -- position 代表调用这个函数的位置, 用于标识
    send_commit_text = send_commit_text or false
    local success, error_msg = pcall(function()
        local current_time = get_current_time_ms()
        local context = env.engine.context

        -- 构建基础状态数据
        local state_data = {
            messege_type = "state",
            is_composing = context:is_composing(),
            timestamp = current_time,
            switches_option = {}, -- 初始化为空表
            properties = {} -- 初始化属性表
        }
        
        if command_key then
            -- 发送字符串命令,例如"enter",代表对端将会接收到之后发送一个回车按键
            -- 构建粘贴命令数据
            local command_message = {
                messege_type = "command",
                command = command_key,
                command_value = command_value,
                timestamp = current_time,
                client_id = "lua_tcp_client"
            }
            state_data.command_message = command_message
        end
        if send_commit_text then
            state_data.messege_type = "commit"
            state_data.current_app = context:get_property("client_app")
            -- 发送上屏内容
            state_data.commit_pinyin = context.input
            state_data.commit_text = context:get_commit_text()
        end

        if position == "unhandled_key_notifier" then
            state_data.messege_type = "commit"
            state_data.current_app = context:get_property("client_app")
            state_data.commit_pinyin = char
            state_data.commit_text = char
        end

        if option_info then
            -- 构建完整的带有option当前配置的状态数据
            local simple_switches = {"ascii_punct"}
            for _, switch_name in ipairs(simple_switches) do
                local switch_state = context:get_option(switch_name)
                table.insert(state_data.switches_option, {
                    name = switch_name,
                    type = "simple",
                    state = switch_state,
                    state_index = switch_state and 1 or 0
                })
            end
        end

        -- 构建属性数据（始终发送）
        local property_names = {"keepon_chat_trigger"}
        for _, property_name in ipairs(property_names) do
            local property_value = context:get_property(property_name)
            table.insert(state_data.properties, {
                name = property_name,
                type = "string",
                value = property_value
            })
        end

        -- 序列化状态数据
        local json_data = json.encode(state_data)

        logger.info("json_data: " .. json_data)

        -- 写入Rime状态服务TCP套接字
        tcp_socket_sync.write_to_rime_socket(json_data)

        -- 处理来自Rime状态服务端的数据
        if socket_system.is_initialized and socket_system.rime_state.is_connected then
            tcp_socket_sync.process_rime_socket_data(env, timeout)
        end
    end)

    if not success then
        logger.error("状态更新失败: " .. tostring(error_msg))
        return false
    end

    return true
end

-- 统计信息
function tcp_socket_sync.get_stats()
    local stats = {
        is_initialized = socket_system.is_initialized,
        host = socket_system.host,

        -- Rime状态服务统计
        rime_state = {
            port = socket_system.rime_state.port,
            is_connected = socket_system.rime_state.is_connected,
            connection_failures = socket_system.rime_state.connection_failures,
            write_failure_count = socket_system.rime_state.write_failure_count,
            timeout = socket_system.rime_state.timeout
        },

        -- AI转换服务统计
        ai_convert = {
            port = socket_system.ai_convert.port,
            is_connected = socket_system.ai_convert.is_connected,
            connection_failures = socket_system.ai_convert.connection_failures,
            write_failure_count = socket_system.ai_convert.write_failure_count,
            timeout = socket_system.ai_convert.timeout
        }
    }

    return stats
end

-- 公开接口：获取连接信息
function tcp_socket_sync.get_connection_info()
    return {
        host = socket_system.host,
        rime_state = {
            port = socket_system.rime_state.port,
            is_connected = socket_system.rime_state.is_connected
        },
        ai_convert = {
            port = socket_system.ai_convert.port,
            is_connected = socket_system.ai_convert.is_connected
        }
    }
end

-- 公开接口：检查双端口系统是否就绪（任一服务可用即为就绪）
function tcp_socket_sync.is_system_ready()
    return socket_system.is_initialized and
               (socket_system.rime_state.is_connected or socket_system.ai_convert.is_connected)
end

-- 公开接口：检查Rime状态服务连接状态
function tcp_socket_sync.is_rime_socket_ready()
    return socket_system.is_initialized and socket_system.rime_state.is_connected
end

-- 公开接口：检查AI转换服务连接状态
function tcp_socket_sync.is_ai_socket_ready()
    return socket_system.is_initialized and socket_system.ai_convert.is_connected
end

-- 公开接口：强制重置连接状态（用于服务端重启后立即重连）
function tcp_socket_sync.force_reconnect()
    logger.info("强制重置所有TCP连接状态")

    -- 重置连接状态和重连计时器
    socket_system.rime_state.last_connect_attempt = 0
    socket_system.ai_convert.last_connect_attempt = 0
    socket_system.rime_state.connection_failures = 0
    socket_system.ai_convert.connection_failures = 0
    socket_system.rime_state.write_failure_count = 0
    socket_system.ai_convert.write_failure_count = 0

    -- 断开现有连接
    tcp_socket_sync.disconnect_from_server()

    -- 尝试重新连接
    local rime_connected = tcp_socket_sync.connect_to_rime_server()
    local ai_connected = tcp_socket_sync.connect_to_ai_server()

    logger.info("强制重连结果 - Rime:" .. tostring(rime_connected) .. " AI:" .. tostring(ai_connected))

    return rime_connected or ai_connected
end

-- 公开接口：设置连接参数
function tcp_socket_sync.set_connection_params(host, rime_port, ai_port)
    if host then
        socket_system.host = host
    end
    if rime_port then
        socket_system.rime_state.port = rime_port
    end
    if ai_port then
        socket_system.ai_convert.port = ai_port
    end
    logger.debug(
        "连接参数已更新: " .. socket_system.host .. " Rime:" .. socket_system.rime_state.port .. " AI:" ..
            socket_system.ai_convert.port)
end

-- 公开接口：发送转换请求（仅发送，不等待响应）
function tcp_socket_sync.send_convert_request(schema_name, shuru_schema, confirmed_pos_input, long_candidates_table,
    timeout_seconds)
    local timeout = timeout_seconds or socket_system.ai_convert.timeout -- 默认使用AI服务超时时间
    local success, result_or_error = pcall(function()
        local current_time = get_current_time_ms()

        -- 构建要转换的拼音字符串
        local convert_data = {
            messege_type = "convert",
            confirmed_pos_input = confirmed_pos_input,
            schema_name = schema_name,
            shuru_schema = shuru_schema,
            stream_mode = true,
            timestamp = current_time,
            timeout = timeout -- 告知服务端预期的超时时间
        }

        -- 提取long_candidates_table中每个元素的text属性，组成数组
        if long_candidates_table then
            convert_data.candidates_text = {}
            for _, candidate in ipairs(long_candidates_table) do
                table.insert(convert_data.candidates_text, candidate.text)
            end
        end

        -- 序列化状态数据
        local json_data = json.encode(convert_data)
        logger.debug("发送转换请求json_data: " .. tostring(json_data))

        if json_data then
            -- 写入AI转换服务TCP套接字
            local result = tcp_socket_sync.write_to_ai_socket(json_data)
            if result then
                logger.debug("转换请求发送成功")
                return true
            else
                logger.debug("转换请求发送失败")
                return false
            end
        else
            logger.debug("convert_data序列化失败,请排查错误: " .. tostring(convert_data))
            return false
        end
    end)

    if not success then
        logger.error("发送转换请求失败: " .. tostring(result_or_error))
        return false
    end

    return result_or_error
end

-- 公开接口：读取转换结果（流式读取，类似AI助手的读取方式）
function tcp_socket_sync.read_convert_result(timeout_seconds)
    local timeout = timeout_seconds or 0.1 -- 默认100ms超时，适合流式读取

    -- 使用现有的read_latest_from_ai_socket函数
    local stream_result = tcp_socket_sync.read_latest_from_ai_socket(timeout)

    if stream_result and stream_result.status == "success" and stream_result.data then
        local parsed_data = stream_result.data

        -- 检查是否是转换结果
        if parsed_data.messege_type == "convert_result_stream" then
            logger.debug("读取到转换结果数据")

            -- 从服务端数据中获取 is_final 状态
            local is_final = parsed_data.is_final or false
            local is_partial = parsed_data.is_partial or false
            local is_timeout = parsed_data.is_timeout or false
            local is_error = parsed_data.is_error or false

            logger.debug("转换结果状态 - is_final: " .. tostring(is_final) .. ", is_partial: " ..
                             tostring(is_partial) .. ", is_timeout: " .. tostring(is_timeout) .. ", is_error: " ..
                             tostring(is_error))

            return {
                status = "success",
                data = parsed_data,
                is_final = is_final,
                is_partial = is_partial,
                is_timeout = is_timeout,
                is_error = is_error
            }
        else
            logger.debug("收到非转换结果数据，类型: " .. tostring(parsed_data.messege_type))
            return {
                status = "no_data",
                data = nil,
                is_final = false
            }
        end
    elseif stream_result and stream_result.status == "timeout" then
        logger.debug("转换结果读取超时(正常) - 服务端可能还没处理完成")
        return {
            status = "timeout",
            data = nil,
            is_final = false
        }
    elseif stream_result and stream_result.status == "error" then
        logger.error("转换结果读取错误: " .. tostring(stream_result.error_msg))
        return {
            status = "error",
            data = nil,
            is_final = true,
            error_msg = stream_result.error_msg
        }
    else
        logger.debug("未知的转换结果读取状态")
        return {
            status = "no_data",
            data = nil,
            is_final = false
        }
    end
end

-- 公开接口：发送粘贴命令到服务端（跨平台通用）
function tcp_socket_sync.send_paste_command(env)
    local success, error_msg = pcall(function()
        local current_time = get_current_time_ms()

        -- 构建粘贴命令数据
        local paste_command = {
            messege_type = "command", -- 使用state类型以兼容现有处理逻辑
            command = "paste", -- 粘贴命令
            timestamp = current_time,
            client_id = "lua_tcp_client"
        }

        -- 序列化命令数据
        local json_data = json.encode(paste_command)
        logger.debug("发送粘贴命令json_data: " .. tostring(json_data))

        if json_data then
            -- 写入Rime状态服务TCP套接字
            local send_success = tcp_socket_sync.write_to_rime_socket(json_data)
            if send_success then
                logger.info("🍴 粘贴命令发送成功，等待服务端执行")

                -- 可选：等待服务端响应
                local response = tcp_socket_sync.process_rime_socket_data(env)
                if response then
                    logger.info("📥 收到粘贴命令执行响应")
                    return true
                else
                    logger.warn("⚠️ 未收到粘贴命令执行响应")
                    return true -- 命令已发送，视为成功
                end
            else
                logger.error("❌ 粘贴命令发送失败")
                return false
            end
        else
            logger.error("粘贴命令序列化失败: " .. tostring(paste_command))
            return false
        end
    end)

    if not success then
        logger.error("发送粘贴命令失败: " .. tostring(error_msg))
        return false
    end

    return true
end

-- 公开接口：发送对话消息到AI服务（仅发送）
function tcp_socket_sync.send_chat_message(commit_text, assistant_id, response_key)
    local success, error_msg = pcall(function()
        local current_time = get_current_time_ms()

        -- 构建对话消息数据
        local chat_data = {
            messege_type = "chat",
            commit_text = commit_text, -- 对话内容
            assistant_id = assistant_id, -- AI对话类型
            -- response_key = response_key,
            timestamp = current_time
        }

        if response_key then
            chat_data.response_key = response_key
        end

        -- 序列化聊天数据
        local json_data = json.encode(chat_data)
        logger.debug("发送对话消息json_data: " .. tostring(json_data))

        if json_data then
            -- 写入AI转换服务TCP套接字
            tcp_socket_sync.write_to_ai_socket(json_data)
            logger.debug("对话消息发送成功，类型: " .. tostring(assistant_id))
        else
            logger.error("对话消息序列化失败: " .. tostring(chat_data))
            return false
        end
    end)

    if not success then
        logger.error("发送对话消息失败: " .. tostring(error_msg))
        return false
    end

    return true
end

-- 初始化系统
function tcp_socket_sync.init()
    logger.info("双端口TCP套接字状态同步系统初始化")

    -- 检查是否已经初始化
    logger.info("socket_system.is_initialized: " .. tostring(socket_system.is_initialized))
    if socket_system.is_initialized then
        return true
    end

    logger.clear()

    -- 尝试连接到Rime状态服务
    local rime_connected = tcp_socket_sync.connect_to_rime_server()
    -- 尝试连接到AI转换服务
    local ai_connected = tcp_socket_sync.connect_to_ai_server()

    if rime_connected or ai_connected then
        socket_system.is_initialized = true
        logger.info("双端口TCP套接字系统初始化成功")
        if rime_connected then
            logger.info("Rime状态服务连接成功")
        end
        if ai_connected then
            logger.info("AI转换服务连接成功")
        end
        logger.info("双端口TCP套接字系统初始化完成")
        return true
    end

    logger.info("双端口TCP套接字系统初始化失败，但系统仍可工作（离线模式）")
    socket_system.is_initialized = true -- 允许离线工作
    logger.info("双端口TCP套接字系统初始化完成")
    return true
end

-- 清理资源
function tcp_socket_sync.fini()
    logger.info("双端口TCP套接字系统清理")

    -- 断开所有TCP连接
    tcp_socket_sync.disconnect_from_server()

    logger.info("双端口TCP套接字系统清理完成")
end

return tcp_socket_sync
