-- Rime输入法日志工具模块
-- 提供统一的日志记录功能
--
-- 使用方法：
-- 1. 分离模式（默认）：每个模块使用独立的日志文件
--    将调用文件中的 config.unique_file_log 设置为 false
-- 2. 统一模式：所有模块输出到同一个日志文件
--    将调用文件中的 config.unique_file_log 设置为 true
--    可通过 config.unique_file_log_file 自定义文件名
-- 3. 控制台输出：可以通过 config.console_output 设置为 true
--    来同时将日志输出到控制台，便于调试
-- 4. 日志级别控制：通过 logger.set_log_level() 设置日志输出级别
--    可用级别：DEBUG < INFO < WARN < ERROR
--    只有大于等于设置级别的日志才会被输出
-- 5. 行号显示：通过 logger.set_show_line_info() 控制是否显示行号
--    默认为 true，日志格式: [时间] [级别] [模块名:行号] 消息
--
-- 全局超级开关（优先级最高）：
-- - logger.set_global_enabled(enabled): 全局日志开关
--   当设置为 true/false 时，强制覆盖所有调用文件的 enabled 设置
--   当设置为 nil 时，使用各个调用文件的 enabled 设置
-- - logger.set_global_unique_file_log(enabled, filename): 全局统一文件开关
--   当设置为 true/false 时，强制覆盖所有调用文件的 unique_file_log 设置
--   当设置为 nil 时，使用各个调用文件的 unique_file_log 设置
--
-- 配置优先级（从高到低）：
-- 1. 全局超级开关（logger.lua 中的 global_overrides）
-- 2. 调用文件中的 config 参数
-- 3. logger.lua 中的 default_config 默认值

local logger = {}

-- 日志级别定义
local LOG_LEVELS = {
    DEBUG = 1,
    INFO = 2,
    WARN = 3,
    ERROR = 4
}

-- 全局超级开关（优先级最高）
local global_overrides = {
    force_enabled = nil,  -- 全局日志开关：nil=不强制, true=强制开启, false=强制关闭
    force_unique_file_log = nil  -- 全局统一文件开关：nil=不强制, true=强制统一文件, false=强制分离文件
}

-- 根据平台获取默认日志目录，优先放在当前用户目录下
local function get_default_log_dir()
    local is_windows = package.config:sub(1, 1) == "\\"

    if is_windows then
        local base_dir = os.getenv("APPDATA")
        if not base_dir or base_dir == "" then
            local userprofile = os.getenv("USERPROFILE")
            if userprofile and userprofile ~= "" then
                base_dir = userprofile .. "\\AppData\\Roaming"
            else
                base_dir = "."
            end
        end

        return base_dir .. "\\Rime\\log\\"
    end

    local home_dir = os.getenv("HOME") or "."
    return home_dir .. "/Library/Aipara/log/"
end

-- 默认配置
local default_config = {
    enabled = true,
    log_dir = get_default_log_dir(),
    timestamp_format = "%Y-%m-%d %H:%M:%S",
    unique_file_log = false,  -- 是否统一输出到同一个日志文件（普通参数，由各个文件自己控制）
    unique_file_log_file = "all_modules.log",  -- 统一日志文件名
    console_output = false,  -- 是否同时输出到控制台
    log_level = "DEBUG",  -- 日志输出级别：DEBUG, INFO, WARN, ERROR
    show_line_info = true  -- 是否显示文件名和行号信息
}

-- 配置管理函数
function logger.set_global_enabled(enabled)
    global_overrides.force_enabled = enabled
end

function logger.set_global_unique_file_log(enabled, filename)
    global_overrides.force_unique_file_log = enabled
    if filename then
        default_config.unique_file_log_file = filename
    end
end

function logger.set_unified_mode(enabled, filename)
    global_overrides.force_unique_file_log = enabled
    if filename then
        default_config.unique_file_log_file = filename
    end
end

function logger.set_console_output(enabled)
    default_config.console_output = enabled
end

function logger.set_log_level(level)
    if LOG_LEVELS[level] then
        default_config.log_level = level
    else
        error("无效的日志级别: " .. tostring(level) .. "。可用级别: DEBUG, INFO, WARN, ERROR")
    end
end

function logger.set_show_line_info(enabled)
    default_config.show_line_info = enabled
end

function logger.get_config()
    return default_config
end

function logger.get_global_overrides()
    return global_overrides
end

-- 创建日志记录器
function logger.create(module_name, config)
    config = config or {}
    
    -- 合并配置
    local log_config = {}
    for k, v in pairs(default_config) do
        if config[k] ~= nil then
            log_config[k] = config[k]
        else
            log_config[k] = v
        end
    end
    
    -- 应用全局超级开关（优先级最高）
    -- 1. 全局日志开关
    if global_overrides.force_enabled ~= nil then
        log_config.enabled = global_overrides.force_enabled
    end
    
    -- 2. 全局统一文件开关
    if global_overrides.force_unique_file_log ~= nil then
        log_config.unique_file_log = global_overrides.force_unique_file_log
    end
    
    -- 生成日志文件路径
    local log_file_path
    if log_config.unique_file_log then
        -- 统一模式：所有模块使用同一个日志文件
        log_file_path = log_config.log_dir .. log_config.unique_file_log_file
    else
        -- 分离模式：每个模块使用独立的日志文件
        log_file_path = log_config.log_dir .. module_name .. ".log"
    end
    
    -- 返回日志记录器对象
    local log_instance = {
        enabled = log_config.enabled,
        module_name = module_name,
        log_file_path = log_file_path,
        timestamp_format = log_config.timestamp_format,
        unique_file_log = log_config.unique_file_log,
        console_output = log_config.console_output,
        log_level = log_config.log_level,
        show_line_info = log_config.show_line_info
    }
    
    -- 清空日志文件函数
    function log_instance.clear()
        if not log_instance.enabled then
            return true
        end
        
        local success, error_msg = pcall(function()
            local file = io.open(log_instance.log_file_path, "w")
            if file then
                file:close()
                return true
            else
                error("无法打开文件进行写入: " .. log_instance.log_file_path)
            end
        end)
        
        if success then
            print("日志文件已清空: " .. log_instance.log_file_path)
            return true
        else
            print("清空日志文件失败: " .. tostring(error_msg))
            return false
        end
    end
    
    -- 写入日志函数
    function log_instance.write(message, level)
        -- -- 打印log_instance中的属性值到日志文件: 
        -- -- 为了避免无限递归，先检查是否已经在记录属性
        -- if not log_instance._logging_properties then
        --     log_instance._logging_properties = true
            
        --     -- 写入属性到日志文件
        --     local properties_info = string.format("log_instance属性: enabled=%s, module_name=%s, log_file_path=%s, timestamp_format=%s",
        --         tostring(log_instance.enabled), tostring(log_instance.module_name), 
        --         tostring(log_instance.log_file_path), tostring(log_instance.timestamp_format))
            
        --     -- 直接写入文件，避免递归调用
        --     local timestamp = os.date(log_instance.timestamp_format)
        --     local property_log_message = string.format("[%s] [DEBUG] [%s] %s\n", 
        --         timestamp, log_instance.module_name, properties_info)
            
        --     local file = io.open(log_instance.log_file_path, "a")
        --     if file then
        --         file:write(property_log_message)
        --         file:close()
        --     end
            
        --     log_instance._logging_properties = false
        -- end
        
        -- 如果日志功能未开启，直接返回
        if not log_instance.enabled then
            return
        end
        
        -- 检查日志级别过滤
        level = level or "INFO"
        local current_level_value = LOG_LEVELS[log_instance.log_level] or LOG_LEVELS["INFO"]
        local message_level_value = LOG_LEVELS[level] or LOG_LEVELS["INFO"]
        
        -- 如果消息级别低于当前设置的级别，不输出
        if message_level_value < current_level_value then
            return
        end
        
        -- 如果message是nil，替换成空字符串
        if message == nil then
            message = ""
        end
        
        -- 获取调用者的文件名和行号信息
        local location_info = ""
        local actual_module_name = log_instance.module_name  -- 默认使用传入的模块名
        
        if log_instance.show_line_info then
            local caller_info = debug.getinfo(3, "Sl")  -- 3级调用栈：write <- info/debug/warn/error <- 实际调用者
            if caller_info and caller_info.source then
                -- 从完整路径中提取文件名（去掉路径和扩展名）
                local source = caller_info.source
                if source:sub(1, 1) == "@" then
                    source = source:sub(2)  -- 去掉开头的@符号
                end
                
                -- 提取文件名
                local filename = source:match("([^/\\]+)$") or source  -- 提取最后的文件名部分
                if filename:match("%.lua$") then
                    actual_module_name = filename:sub(1, -5)  -- 去掉.lua扩展名
                else
                    actual_module_name = filename
                end
                
                if caller_info.currentline and caller_info.currentline > 0 then
                    location_info = string.format(":%d", caller_info.currentline)
                end
            end
        end
        
        level = level or "INFO"
        local timestamp = os.date(log_instance.timestamp_format)
        local log_message = string.format("[%s] [%s] [%s%s] %s\n", 
            timestamp, level, actual_module_name, location_info, message)
        
        -- 如果启用了控制台输出，同时输出到控制台
        if log_instance.console_output then
            print(string.format("[%s] [%s] [%s%s] %s", 
                timestamp, level, actual_module_name, location_info, message))
        end
        
        -- 写入到文件
        local success, error_msg = pcall(function()
            local file = io.open(log_instance.log_file_path, "a")
            if file then
                file:write(log_message)
                file:close()
            else
                error("无法打开日志文件: " .. log_instance.log_file_path)
            end
        end)
        
        if not success then
            local error_info = "写入日志失败: " .. tostring(error_msg)
            print(error_info)
        end
    end
    
    -- 便捷的日志级别函数
    function log_instance.info(message)
        log_instance.write(message, "INFO")
    end
    
    function log_instance.debug(message)
        log_instance.write(message, "DEBUG")
    end
    
    function log_instance.warn(message)
        log_instance.write(message, "WARN")
    end
    
    function log_instance.error(message)
        log_instance.write(message, "ERROR")
    end
    
    return log_instance
end

return logger
