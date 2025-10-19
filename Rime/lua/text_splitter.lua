-- text_splitter.lua - 文本智能切分模块
-- 用于处理反引号和标点符号的智能切分
local text_splitter = {}

-- 模块级别的配置变量
text_splitter.english_mode_symbol = "`" -- 默认值为反引号

-- 配置更新函数
function text_splitter.update_current_config(config)
    -- 读取英文模式符号配置
    local symbol = config:get_string("translator/english_mode_symbol")
    if symbol and symbol ~= "" then
        text_splitter.english_mode_symbol = symbol
    end
end

-- 英文标点符号到中文标点符号的映射表
local punct_map = {
    [","] = "，", -- 逗号
    ["."] = "。", -- 句号
    ["?"] = "？", -- 问号  
    ["!"] = "！", -- 感叹号
    [":"] = "：", -- 冒号
    [";"] = "；", -- 分号
    ["("] = "（", -- 左括号
    [")"] = "）", -- 右括号
    -- ["["] = "【",    -- 左方括号
    -- ["]"] = "】",    -- 右方括号
    ["{"] = "｛", -- 左花括号
    ["}"] = "｝", -- 右花括号
    ["<"] = "《", -- 左书名号
    [">"] = "》" -- 右书名号
}

-- 定义需要转换为普通字符的按键
text_splitter.handle_keys = {
    ["space"] = " ", -- 空格转为空格字符
    -- 数字键
    ["1"] = "1",
    ["2"] = "2",
    ["3"] = "3",
    ["4"] = "4",
    ["5"] = "5",
    ["6"] = "6",
    ["7"] = "7",
    ["8"] = "8",
    ["9"] = "9",
    ["0"] = "0",
    -- 数字键的Shift版本（符号）
    ["Shift+1"] = "!", -- !
    ["Shift+2"] = "@", -- @
    ["Shift+3"] = "#", -- #
    ["Shift+4"] = "$", -- $
    ["Shift+5"] = "%", -- %
    ["Shift+6"] = "^", -- ^
    ["Shift+7"] = "&", -- &
    ["Shift+8"] = "*", -- *
    ["Shift+9"] = "(", -- (
    ["Shift+0"] = ")", -- )

    -- 标点符号（不需要Shift）
    ["period"] = ".", -- 句号
    ["comma"] = ",", -- 逗号
    ["semicolon"] = ";", -- 分号
    ["apostrophe"] = "'", -- 单引号/撇号
    ["bracketleft"] = "[", -- 左方括号
    ["bracketright"] = "]", -- 右方括号
    ["hyphen"] = "-", -- 连字符
    ["equal"] = "=", -- 等号
    ["slash"] = "/", -- 斜杠
    ["backslash"] = "\\", -- 反斜杠
    ["grave"] = "`", -- 反引号

    -- 标点符号的Shift版本
    ["Shift+semicolon"] = ":", -- :
    ["Shift+apostrophe"] = "\"", -- "
    ["Shift+bracketleft"] = "{", -- {
    ["Shift+bracketright"] = "}", -- }
    ["Shift+hyphen"] = "_", -- _
    ["Shift+equal"] = "+", -- +
    ["Shift+slash"] = "?", -- ?
    ["Shift+backslash"] = "|", -- |
    ["Shift+grave"] = "~", -- ~

    -- 直接映射的符号键
    ["minus"] = "-", -- 冒号
    ["colon"] = ":", -- 冒号
    ["question"] = "?", -- 问号
    ["exclam"] = "!", -- 感叹号
    ["quotedbl"] = "\"", -- 双引号
    ["parenleft"] = "(", -- 左圆括号
    ["parenright"] = ")", -- 右圆括号
    ["braceleft"] = "{", -- 左花括号
    ["braceright"] = "}", -- 右花括号
    ["underscore"] = "_", -- 下划线
    ["plus"] = "+", -- 加号
    ["asterisk"] = "*", -- 星号
    ["at"] = "@", -- @ 符号
    ["numbersign"] = "#", -- # 号
    ["dollar"] = "$", -- 美元符号
    ["percent"] = "%", -- 百分号
    ["ampersand"] = "&", -- & 符号
    ["less"] = "<", -- 小于号
    ["greater"] = ">", -- 大于号
    ["asciitilde"] = "~", -- 波浪号
    ["asciicircum"] = "^", -- 插入符号
    ["bar"] = "|", -- 竖线

    -- 为这些符号键也添加Shift版本（以防万一）
    ["Shift+colon"] = ":",
    ["Shift+question"] = "?",
    ["Shift+exclam"] = "!",
    ["Shift+quotedbl"] = "\"",
    ["Shift+parenleft"] = "(",
    ["Shift+parenright"] = ")",
    ["Shift+braceleft"] = "{",
    ["Shift+braceright"] = "}",
    ["Shift+underscore"] = "_",
    ["Shift+plus"] = "+",
    ["Shift+asterisk"] = "*",
    ["Shift+at"] = "@",
    ["Shift+numbersign"] = "#",
    ["Shift+dollar"] = "$",
    ["Shift+percent"] = "%",
    ["Shift+ampersand"] = "&",
    ["Shift+less"] = "<",
    ["Shift+greater"] = ">",
    ["Shift+asciitilde"] = "~",
    ["Shift+asciicircum"] = "^",
    ["Shift+bar"] = "|"

}

-- 定义需要转换为普通字符的按键
text_splitter.send_chars = {
    ["BackSpace"] = "<BACKSPACE>",
    ["Return"] = "<Enter>",
    ["space"] = "<space>",
    -- 字母键
    ["a"] = "a",
    ["b"] = "b",
    ["c"] = "c",
    ["d"] = "d",
    ["e"] = "e",
    ["f"] = "f",
    ["g"] = "g",
    ["h"] = "h",
    ["i"] = "i",
    ["j"] = "j",
    ["k"] = "k",
    ["l"] = "l",
    ["m"] = "m",
    ["n"] = "n",
    ["o"] = "o",
    ["p"] = "p",
    ["q"] = "q",
    ["r"] = "r",
    ["s"] = "s",
    ["t"] = "t",
    ["u"] = "u",
    ["v"] = "v",
    ["w"] = "w",
    ["x"] = "x",
    ["y"] = "y",
    ["z"] = "z",
    -- 数字键
    ["1"] = "1",
    ["2"] = "2",
    ["3"] = "3",
    ["4"] = "4",
    ["5"] = "5",
    ["6"] = "6",
    ["7"] = "7",
    ["8"] = "8",
    ["9"] = "9",
    ["0"] = "0",
    -- 数字键的Shift版本（符号）
    ["Shift+1"] = "!", -- !
    ["Shift+2"] = "@", -- @
    ["Shift+3"] = "#", -- #
    ["Shift+4"] = "$", -- $
    ["Shift+5"] = "%", -- %
    ["Shift+6"] = "^", -- ^
    ["Shift+7"] = "&", -- &
    ["Shift+8"] = "*", -- *
    ["Shift+9"] = "(", -- (
    ["Shift+0"] = ")", -- )

    -- 标点符号（不需要Shift）
    ["period"] = ".", -- 句号
    ["comma"] = ",", -- 逗号
    ["semicolon"] = ";", -- 分号
    ["apostrophe"] = "'", -- 单引号/撇号
    ["bracketleft"] = "[", -- 左方括号
    ["bracketright"] = "]", -- 右方括号
    ["hyphen"] = "-", -- 连字符
    ["equal"] = "=", -- 等号
    ["slash"] = "/", -- 斜杠
    ["backslash"] = "\\", -- 反斜杠
    ["grave"] = "`", -- 反引号

    -- 标点符号的Shift版本
    ["Shift+semicolon"] = ":", -- :
    ["Shift+apostrophe"] = "\"", -- "
    ["Shift+bracketleft"] = "{", -- {
    ["Shift+bracketright"] = "}", -- }
    ["Shift+hyphen"] = "_", -- _
    ["Shift+equal"] = "+", -- +
    ["Shift+slash"] = "?", -- ?
    ["Shift+backslash"] = "|", -- |
    ["Shift+grave"] = "~", -- ~

    -- 直接映射的符号键
    ["minus"] = "-", -- 冒号
    ["colon"] = ":", -- 冒号
    ["question"] = "?", -- 问号
    ["exclam"] = "!", -- 感叹号
    ["quotedbl"] = "\"", -- 双引号
    ["parenleft"] = "(", -- 左圆括号
    ["parenright"] = ")", -- 右圆括号
    ["braceleft"] = "{", -- 左花括号
    ["braceright"] = "}", -- 右花括号
    ["underscore"] = "_", -- 下划线
    ["plus"] = "+", -- 加号
    ["asterisk"] = "*", -- 星号
    ["at"] = "@", -- @ 符号
    ["numbersign"] = "#", -- # 号
    ["dollar"] = "$", -- 美元符号
    ["percent"] = "%", -- 百分号
    ["ampersand"] = "&", -- & 符号
    ["less"] = "<", -- 小于号
    ["greater"] = ">", -- 大于号
    ["asciitilde"] = "~", -- 波浪号
    ["asciicircum"] = "^", -- 插入符号
    ["bar"] = "|", -- 竖线

    -- 为这些符号键也添加Shift版本（以防万一）
    ["Shift+colon"] = ":",
    ["Shift+question"] = "?",
    ["Shift+exclam"] = "!",
    ["Shift+quotedbl"] = "\"",
    ["Shift+parenleft"] = "(",
    ["Shift+parenright"] = ")",
    ["Shift+braceleft"] = "{",
    ["Shift+braceright"] = "}",
    ["Shift+underscore"] = "_",
    ["Shift+plus"] = "+",
    ["Shift+asterisk"] = "*",
    ["Shift+at"] = "@",
    ["Shift+numbersign"] = "#",
    ["Shift+dollar"] = "$",
    ["Shift+percent"] = "%",
    ["Shift+ampersand"] = "&",
    ["Shift+less"] = "<",
    ["Shift+greater"] = ">",
    ["Shift+asciitilde"] = "~",
    ["Shift+asciicircum"] = "^",
    ["Shift+bar"] = "|"

}

-- 成对引号的映射表
local quote_map = {
    ["\""] = {"“", "”"}, -- 双引号：前引号、后引号
    ["'"] = {"‘", "’"} -- 单引号：前引号、后引号
}

-- 从中文英文混合字符串中切分出索引段, 前闭合后闭合
function text_splitter.utf8_utils_sub(str, start_char, end_char)
    local char_len = utf8.len(str)

    -- 处理默认值
    start_char = start_char or 1
    end_char = end_char or char_len

    -- 处理负索引
    if start_char < 0 then
        start_char = char_len + start_char + 1
    end
    if end_char < 0 then
        end_char = char_len + end_char + 1
    end

    -- 边界检查
    start_char = math.max(1, start_char)
    end_char = math.min(char_len, end_char)

    if start_char > end_char then
        return ""
    end

    local start_byte = utf8.offset(str, start_char)
    local end_byte = utf8.offset(str, end_char + 1)

    if not start_byte then
        return ""
    end

    if not end_byte then
        return string.sub(str, start_byte)
    else
        return string.sub(str, start_byte, end_byte - 1)
    end
end

-- 处理成对引号的替换函数, 如果最后替换的是一个单数个, 将记录返回
function text_splitter.replace_quotes_record_single(text, double_quote_open)

    local result = text

    -- 处理双引号
    -- local double_quote_open = true -- 跟踪双引号状态，true表示下一个是开引号
    result = result:gsub("\"", function()
        if double_quote_open then
            double_quote_open = false
            return "“" -- 前引号
        else
            double_quote_open = true
            return "”" -- 后引号
        end
    end)

    -- -- 处理单引号, 因为单引号是音节分隔符, 所以这里不能使用单引号. 
    -- local single_quote_open = true  -- 跟踪单引号状态，true表示下一个是开引号
    -- result = result:gsub("'", function()
    --     if single_quote_open then
    --         single_quote_open = false
    --         return "‘"  -- 前引号
    --     else
    --         single_quote_open = true
    --         return "’"  -- 后引号
    --     end
    -- end)

    return result, double_quote_open
end

-- 处理成对引号的替换函数
function text_splitter.replace_quotes(text)

    local result = text

    -- 处理双引号
    local double_quote_open = true -- 跟踪双引号状态，true表示下一个是开引号
    result = result:gsub("\"", function()
        if double_quote_open then
            double_quote_open = false
            return "“" -- 前引号
        else
            double_quote_open = true
            return "”" -- 后引号
        end
    end)

    -- -- 处理单引号, 因为单引号是音节分隔符, 所以这里不能使用单引号. 
    -- local single_quote_open = true  -- 跟踪单引号状态，true表示下一个是开引号
    -- result = result:gsub("'", function()
    --     if single_quote_open then
    --         single_quote_open = false
    --         return "‘"  -- 前引号
    --     else
    --         single_quote_open = true
    --         return "’"  -- 后引号
    --     end
    -- end)

    return result
end

-- 标点符号替换函数
function text_splitter.replace_punct(text)
    if not text or text == "" then
        return text
    end

    local result = text

    -- 先处理成对引号
    result = text_splitter.replace_quotes(result)

    -- 再处理其他标点符号
    for eng_punct, chn_punct in pairs(punct_map) do
        result = result:gsub(eng_punct:gsub("([%(%)%.%+%-%*%?%[%]%^%$%%])", "%%%1"), chn_punct)
    end

    return result
end

-- 标点符号替换函数, 对于传递进来的坐标范围进行替换
function text_splitter.replace_punct_skip_pos(text, chinese_pos_str, logger)
    -- chinese_pos = "chinese_pos:3,6,"
    -- chinese_pos = "chinese_pos:1,1,9,12,"
    -- 我希望对于text中的标点符号进行替换, chinese_pos中是一个字符串,判断chinese_pos应该以"chinese_pos:"开头, 然后后面每一对数字代表了中文字符的坐标
    -- 只有当坐标位置在中文字符范围内的时候,才将标点符号从英文替换成中文标点符号.
    -- 对于英文双引号,如果是第一次遇到则替换成中文前引号“, 第二次遇到替换成中文后引号”.

    -- 检查坐标字符串格式
    if not chinese_pos_str or not chinese_pos_str:match("^chinese_pos:") then
        logger.info("坐标字符串格式不正确或为空，不进行替换")
        return
    end
    -- 解析坐标范围
    local ranges = {}
    local pos_data = chinese_pos_str:gsub("^chinese_pos:", "")
    -- 一次性匹配两个数字作为一对
    for start_num, end_num in pos_data:gmatch("(%d+),(%d+)") do
        table.insert(ranges, {
            start = tonumber(start_num),
            _end = tonumber(end_num)
        })
    end

    local final_text = ""
    local last_end_num = 0
    local double_quote_open = true

    local chinese_first = false
    for i, range in ipairs(ranges) do
        local start_num = range.start
        local end_num = range._end
        logger.info("start_num: " .. start_num .. " end_num: " .. end_num)

        -- 如果是第一段, 如果不是从1开始的,说明前边是英文段. 如果是从1开始的,则不用判断前边英文段了
        -- 如果第一段是中文, 那么对于后面的英文来说,第一段应该不存在
        local english_str = ""
        if start_num == 1 then
            -- 说明是从中文开始的, 不需要处理英文段
            chinese_first = true
        else
            -- 两种情况会进入这里, 第一种情况: 从英文开始的, 这种情况下chinese_first = false,那么应该从1开始取到这里
            if not chinese_first then
                english_str = text_splitter.utf8_utils_sub(text, 1, start_num - 1)
                logger.info("english_str: " .. english_str)
            else
                -- 进入这里说明, 第一段是中文, start_num ~= 1 那么一定不是第一段中文, 这时候应该使用上一段的结尾和这一段的开头
                english_str = text_splitter.utf8_utils_sub(text, last_end_num + 1, start_num - 1)
                logger.info("english_str: " .. english_str)
            end
            final_text = final_text .. english_str
            logger.info("final_text: " .. final_text)

        end

        local chinese_str = text_splitter.utf8_utils_sub(text, start_num, end_num)

        if text_splitter.has_punctuation_no_rawenglish(chinese_str, logger) then
            for eng_punct, chn_punct in pairs(punct_map) do
                chinese_str = chinese_str:gsub(eng_punct:gsub("([%(%)%.%+%-%*%?%[%]%^%$%%])", "%%%1"), chn_punct)
            end
            -- 对引号进行替换
            chinese_str, double_quote_open = text_splitter.replace_quotes_record_single(chinese_str, double_quote_open)
        end

        logger.debug("chinese_str: " .. chinese_str)
        final_text = final_text .. chinese_str
        last_end_num = end_num

    end

    -- 有可能最后一个中文段后面还有英文段, 如何判断呢? 
    if last_end_num < utf8.len(text) then
        local remaining_str = text_splitter.utf8_utils_sub(text, last_end_num + 1, -1)
        final_text = final_text .. remaining_str
    end

    return final_text

end

-- 标点符号替换函数, 对于反引号中间的部分不进行替换
function text_splitter.replace_punct_skip_rawenglish(text, logger)
    if not text or text == "" then
        return text
    end

    local result = text

    -- 1. 首先判断是否存在英文模式符号，如果不存在，就使用原来的搜索方式
    if not string.find(result, text_splitter.english_mode_symbol) then
        logger.info("未发现英文模式符号, 使用原来的标点符号替换模式")
        -- 先处理成对引号
        result = text_splitter.replace_quotes(result)

        -- 再处理其他标点符号
        for eng_punct, chn_punct in pairs(punct_map) do
            result = result:gsub(eng_punct:gsub("([%(%)%.%+%-%*%?%[%]%^%$%%])", "%%%1"), chn_punct)
        end
    else
        -- 如果有英文模式符号，则进入英文模式符号模式, 替换之前首先判断是否在英文模式符号索引范围之内
        logger.info("发现反引号, 使用跳过反引号的标点符号替换模式")

        -- 针对中文字符串的反引号切分功能
        local segments = {} -- 存储切分后的段落

        -- 使用正则表达式切分反引号内容
        local index = 1
        local in_rawenglish = false

        while index <= #result do
            if in_rawenglish then
                -- 查找结束英文模式符号
                local end_index = string.find(result, text_splitter.english_mode_symbol, index)
                if end_index then
                    -- 找到配对的英文模式符号
                    local rawenglish_content = string.sub(result, index - 1, end_index) -- 包含两个英文模式符号
                    table.insert(segments, {
                        type = "rawenglish_combo",
                        content = rawenglish_content
                    })
                    index = end_index + 1
                    in_rawenglish = false
                else
                    -- 没有配对的英文模式符号，剩余部分都是英文模式符号内容
                    local rawenglish_content = string.sub(result, index - 1) -- 包含开始的英文模式符号
                    table.insert(segments, {
                        type = "rawenglish_combo",
                        content = rawenglish_content
                    })
                    break
                end
            else
                -- 查找开始英文模式符号
                local start_pos = string.find(result, text_splitter.english_mode_symbol, index)
                if start_pos then
                    -- 找到英文模式符号，保存之前的普通内容
                    if start_pos > index then
                        local normal_content = string.sub(result, index, start_pos - 1)
                        table.insert(segments, {
                            type = "normal",
                            content = normal_content
                        })
                    end
                    index = start_pos + 1
                    in_rawenglish = true
                else
                    -- 没有更多英文模式符号，剩余部分都是普通内容
                    if index <= #result then
                        local normal_content = string.sub(result, index)
                        table.insert(segments, {
                            type = "normal",
                            content = normal_content
                        })
                    end
                    break
                end
            end
        end

        -- 对每个段落进行处理
        local new_result = ""
        for _, segment in ipairs(segments) do
            if segment.type == "normal" then
                -- 对普通段落进行标点符号替换
                local processed_segment = segment.content

                -- 先处理成对引号
                processed_segment = text_splitter.replace_quotes(processed_segment)

                -- 再处理其他标点符号
                for eng_punct, chn_punct in pairs(punct_map) do
                    processed_segment = processed_segment:gsub(eng_punct:gsub("([%(%)%.%+%-%*%?%[%]%^%$%%])", "%%%1"),
                        chn_punct)
                end

                new_result = new_result .. processed_segment
            else
                -- 英文模式符号段落保持原样
                new_result = new_result .. segment.content
            end
        end

        result = new_result
    end

    return result
end

-- 标点符号替换函数, 原生不替换引号版本
function text_splitter.replace_punct_org(text)
    if not text or text == "" then
        return text
    end

    local result = text
    for eng_punct, chn_punct in pairs(punct_map) do
        result = result:gsub(eng_punct:gsub("([%(%)%.%+%-%*%?%[%]%^%$%%])", "%%%1"), chn_punct)
    end
    return result
end

-- 检测是否包含标点符号
function text_splitter.has_punctuation(text, logger)
    if not text or text == "" then
        return false
    end

    logger.info("检测输入内容是否包含标点符号: " .. text)

    -- 简单检查是否包含常见标点符号
    local has_punct = false

    -- 检查英文标点 (包含反引号)
    if string.find(text, "[,.!?;:()%[%]<>/_=+*&^%%$#@~`|\\-]") then
        has_punct = true
    end

    logger.info("has_punct: " .. tostring(has_punct))

    return has_punct
end

-- 检测是否包含标点符号（不含反引号版本）
function text_splitter.has_punctuation_no_rawenglish(text, logger)
    if not text or text == "" then
        return false
    end

    logger.info("检测输入内容是否包含标点符号(不含反引号): " .. text)

    -- 只检查英文标点（不包含反引号）
    local has_punct = false
    if string.find(text, "[,.!?;:()%[%]<>/_=+*&^%%$#@~|\\-'\"]") then
        has_punct = true
    end

    -- 方法2: 使用哈希表（Set）查找，避免循环
    -- 创建标点符号集合
    local punct_set = {
        ["“"] = true,
        ["”"] = true,
        ["‘"] = true,
        ["’"] = true,
        ["，"] = true,
        ["。"] = true,
        ["？"] = true,
        ["！"] = true,
        ["："] = true,
        ["；"] = true,
        ["（"] = true,
        ["）"] = true,
        ["【"] = true,
        ["】"] = true,
        ["｛"] = true,
        ["｝"] = true,
        ["《"] = true,
        ["》"] = true,
        ["、"] = true,
        ["……"] = true,
        ["—"] = true,
        ["·"] = true,
        ["〈"] = true,
        ["〉"] = true,
        ["「"] = true,
        ["」"] = true,
        ["『"] = true,
        ["』"] = true,
        ["〔"] = true,
        ["〕"] = true,
        ["〖"] = true,
        ["〗"] = true
    }
    -- 遍历文本中的每个UTF-8字符
    for pos, code in utf8.codes(text) do
        local char = utf8.char(code)
        if punct_set[char] then
            has_punct = true
        end
    end

    logger.info("has_punct(no rawenglish): " .. tostring(has_punct))

    return has_punct
end

-- 智能切分输入并转换双拼到全拼
function text_splitter.split_and_convert_input(input, replace_punct_enabled)
    -- 使用默认空分隔符的版本
    return text_splitter.split_and_convert_input_with_delimiter(input, "", "", replace_punct_enabled)
end

-- 带分隔符的智能切分函数
function text_splitter.split_and_convert_input_with_delimiter(input, rawenglish_delimiter_before,
    rawenglish_delimiter_after, replace_punct_enabled)
    rawenglish_delimiter_before = rawenglish_delimiter_before or "" -- 默认无分隔符
    rawenglish_delimiter_after = rawenglish_delimiter_after or "" -- 默认无分隔符
    replace_punct_enabled = replace_punct_enabled or false -- 默认不替换标点符号

    -- 先处理英文模式符号 - 支持多对英文模式符号
    -- nihk`hello`wode`dream3`keyi 应该处理成：nihk + `hello` + wode + `dream3` + keyi
    -- nihk`hello`wode`dream3 应该处理成：nihk + `hello` + wode + `dream3（后面所有内容不处理）
    local rawenglish_positions = {} -- 所有英文模式符号位置

    -- 先找到所有英文模式符号的位置
    for i = 1, #input do
        local char = input:sub(i, i)
        if char == text_splitter.english_mode_symbol then
            table.insert(rawenglish_positions, i)
        end
    end

    -- 检查英文模式符号数量
    local rawenglish_count = #rawenglish_positions
    local has_unpaired_rawenglish = (rawenglish_count % 2 == 1) -- 奇数个英文模式符号表示有未配对的

    -- 定义标点符号模式
    local punct_pattern = "[,.!?;:()%[%]<>/_=+*&^%%$#@~|%-`'\"']"

    -- 切分输入，保留标点符号位置
    local segments = {} -- 片段列表
    local current_segment = "" -- 当前片段
    local i = 1
    local in_rawenglish = false -- 在英文模式符号中
    local rawenglish_content = "" -- 英文模式符号内容
    local rawenglish_pair_index = 0 -- 当前处理到第几个英文模式符号

    while i <= #input do
        local char = input:sub(i, i) -- 当前字符

        -- 检查是否到达未配对的最后一个英文模式符号
        if has_unpaired_rawenglish and rawenglish_pair_index == rawenglish_count - 1 and char ==
            text_splitter.english_mode_symbol then
            -- 最后一个未配对的英文模式符号，从这里开始到末尾都不处理
            if current_segment ~= "" then
                local segment_start = i - #current_segment - 1 -- 转换为0基索引
                table.insert(segments, {
                    type = "abc",
                    content = current_segment,
                    original = current_segment,
                    start = segment_start,
                    _end = i - 1, -- 开区间，不包含当前位置
                    length = #current_segment
                })
                current_segment = ""
            end

            -- 对于未配对的英文模式符号，包装其内容
            local remaining_content = input:sub(i + 1)
            local processed_content = rawenglish_delimiter_before .. remaining_content .. rawenglish_delimiter_after
            table.insert(segments, {
                type = "rawenglish_combo",
                content = processed_content,
                original = text_splitter.english_mode_symbol .. remaining_content,
                start = i - 1, -- 转换为0基索引，从英文模式符号开始
                _end = #input, -- 开区间，到字符串末尾
                length = #input - i + 1
            })
            break
        elseif char == text_splitter.english_mode_symbol then
            -- 不是最后一个未配对的英文模式符号
            rawenglish_pair_index = rawenglish_pair_index + 1
            if not in_rawenglish then
                -- 开始英文模式符号内容
                if current_segment ~= "" then -- 遇到英文模式符号，且之前不是在英文模式符号当中,将之前积累的内容直接添加成片段
                    local segment_start = i - #current_segment - 1 -- 转换为0基索引
                    table.insert(segments, {
                        type = "abc",
                        content = current_segment,
                        original = current_segment,
                        start = segment_start,
                        _end = i - 1, -- 开区间，不包含英文模式符号位置
                        length = #current_segment
                    }) -- 类型=文本，内容
                    current_segment = ""
                end
                in_rawenglish = true
                rawenglish_content = ""
            else
                -- 结束英文模式符号内容，添加分隔符
                local processed_content = rawenglish_delimiter_before .. rawenglish_content ..
                                              rawenglish_delimiter_after
                local rawenglish_start = i - #rawenglish_content - 2 -- 转换为0基索引，包含开始英文模式符号
                table.insert(segments, {
                    type = "rawenglish_combo",
                    content = processed_content,
                    original = text_splitter.english_mode_symbol .. rawenglish_content ..
                        text_splitter.english_mode_symbol,
                    start = rawenglish_start,
                    _end = i, -- 开区间，不包含结束英文模式符号后的位置
                    length = #rawenglish_content + 2
                })
                in_rawenglish = false
                rawenglish_content = ""
            end
        elseif in_rawenglish then
            rawenglish_content = rawenglish_content .. char
        elseif char:match(punct_pattern) then
            -- 遇到标点符号
            if current_segment ~= "" then
                local segment_start = i - #current_segment - 1 -- 转换为0基索引
                table.insert(segments, {
                    type = "abc",
                    content = current_segment,
                    original = current_segment,
                    start = segment_start,
                    _end = i - 1, -- 开区间，不包含标点符号位置
                    length = #current_segment
                }) -- 类型=文本
                current_segment = ""
            end
            table.insert(segments, {
                type = "punct",
                content = replace_punct_enabled and text_splitter.replace_punct(char) or char,
                original = char,
                start = i - 1, -- 转换为0基索引
                _end = i, -- 开区间，不包含下一个字符位置
                length = 1
            }) -- 类型=标点
        else
            current_segment = current_segment .. char
        end

        i = i + 1
    end

    -- 处理最后一个片段
    if in_rawenglish then
        -- 未闭合的英文模式符号内容，添加分隔符
        local processed_content = rawenglish_delimiter_before .. rawenglish_content .. rawenglish_delimiter_after
        local rawenglish_start = #input - #rawenglish_content - 1 -- 转换为0基索引，包含英文模式符号
        table.insert(segments, {
            type = "rawenglish_combo",
            content = processed_content,
            original = text_splitter.english_mode_symbol .. rawenglish_content,
            start = rawenglish_start,
            _end = #input, -- 开区间，到字符串末尾
            length = #rawenglish_content + 1
        })
    elseif current_segment ~= "" then
        local segment_start = #input - #current_segment -- 转换为0基索引
        table.insert(segments, {
            type = "abc",
            content = current_segment,
            original = current_segment,
            start = segment_start,
            _end = #input, -- 开区间，到字符串末尾
            length = #current_segment
        })
    end

    return segments
end

-- 只处理英文模式符号的切分函数
function text_splitter.split_by_rawenglish(input, seg_start, seg_end, delimiter_before, delimiter_after)
    delimiter_before = delimiter_before or "" -- 默认无分隔符
    delimiter_after = delimiter_after or "" -- 默认无分隔符
    seg_start = seg_start or 0 -- 默认起始位置为0

    -- 先找到所有英文模式符号的位置
    local rawenglish_positions = {}
    for i = 1, #input do
        local char = input:sub(i, i)
        if char == text_splitter.english_mode_symbol then
            table.insert(rawenglish_positions, i)
        end
    end

    -- 检查英文模式符号数量
    local rawenglish_count = #rawenglish_positions
    local has_unpaired_rawenglish = (rawenglish_count % 2 == 1) -- 奇数个英文模式符号表示有未配对的

    local segments = {} -- 片段列表
    local current_segment = "" -- 当前片段
    local i = 1
    local in_rawenglish = false -- 在英文模式符号中
    local rawenglish_content = "" -- 英文模式符号内容
    local rawenglish_pair_index = 0 -- 当前处理到第几个英文模式符号

    while i <= #input do
        local char = input:sub(i, i) -- 当前字符

        -- 检查是否到达未配对的最后一个英文模式符号
        if has_unpaired_rawenglish and rawenglish_pair_index == rawenglish_count - 1 and char ==
            text_splitter.english_mode_symbol then
            -- 最后一个未配对的英文模式符号，从这里开始到末尾都不处理
            if current_segment ~= "" then
                local segment_start = seg_start + i - #current_segment - 1 -- 添加seg_start偏移
                table.insert(segments, {
                    type = "abc",
                    content = current_segment,
                    original = current_segment,
                    start = segment_start,
                    _end = seg_start + i - 1, -- 添加seg_start偏移，开区间，不包含英文模式符号位置
                    length = #current_segment
                })
                current_segment = ""
            end

            -- 对于未配对的英文模式符号，包装其内容
            local remaining_content = input:sub(i + 1)
            local processed_content = delimiter_before .. remaining_content .. delimiter_after
            -- 添加原始英文模式符号内容字段
            table.insert(segments, {
                type = "rawenglish_combo",
                content = processed_content,
                original = text_splitter.english_mode_symbol .. remaining_content,
                start = seg_start + i - 1, -- 添加seg_start偏移，从英文模式符号开始
                _end = seg_start + #input, -- 添加seg_start偏移，开区间，到字符串末尾
                length = #input - i + 1
            })
            break
        elseif char == text_splitter.english_mode_symbol then
            -- 不是最后一个未配对的英文模式符号
            rawenglish_pair_index = rawenglish_pair_index + 1
            if not in_rawenglish then
                -- 开始英文模式符号内容
                if current_segment ~= "" then
                    local segment_start = seg_start + i - #current_segment - 1 -- 添加seg_start偏移
                    table.insert(segments, {
                        type = "abc",
                        content = current_segment,
                        original = current_segment,
                        start = segment_start,
                        _end = seg_start + i - 1, -- 添加seg_start偏移，开区间，不包含英文模式符号位置
                        length = #current_segment
                    })
                    current_segment = ""
                end
                in_rawenglish = true
                rawenglish_content = ""
            else
                -- 结束英文模式符号内容，添加分隔符
                local processed_content = delimiter_before .. rawenglish_content .. delimiter_after
                -- 添加原始英文模式符号内容字段
                local rawenglish_start = seg_start + i - #rawenglish_content - 2 -- 添加seg_start偏移，包含开始英文模式符号
                table.insert(segments, {
                    type = "rawenglish_combo",
                    content = processed_content,
                    original = text_splitter.english_mode_symbol .. rawenglish_content ..
                        text_splitter.english_mode_symbol,
                    start = rawenglish_start,
                    _end = seg_start + i, -- 添加seg_start偏移，开区间，不包含结束英文模式符号后的位置
                    length = #rawenglish_content + 2
                })
                in_rawenglish = false
                rawenglish_content = ""
            end
        elseif in_rawenglish then
            rawenglish_content = rawenglish_content .. char
        else
            -- 其他所有字符（包括标点符号）都加入当前段落
            current_segment = current_segment .. char
        end

        i = i + 1
    end

    -- 处理最后一个片段
    if in_rawenglish then
        -- 未闭合的英文模式符号内容，添加分隔符
        local processed_content = delimiter_before .. rawenglish_content .. delimiter_after
        -- 添加原始英文模式符号内容字段
        local rawenglish_start = seg_start + #input - #rawenglish_content - 1 -- 添加seg_start偏移，包含英文模式符号
        table.insert(segments, {
            type = "rawenglish_combo",
            content = processed_content,
            original = text_splitter.english_mode_symbol .. rawenglish_content,
            start = rawenglish_start,
            _end = seg_start + #input, -- 添加seg_start偏移，开区间，到字符串末尾
            length = #rawenglish_content + 1
        })
    elseif current_segment ~= "" then
        local segment_start = seg_start + #input - #current_segment -- 添加seg_start偏移
        table.insert(segments, {
            type = "abc",
            content = current_segment,
            original = current_segment,
            start = segment_start,
            _end = seg_start + #input, -- 添加seg_start偏移，开区间，到字符串末尾
            length = #current_segment
        })
    end

    return segments
end

-- 带日志记录的版本
function text_splitter.split_and_convert_input_with_log(input, logger, replace_punct_enabled)
    logger.info("开始处理输入: " .. input)

    local segments = text_splitter.split_and_convert_input(input, replace_punct_enabled)

    logger.info("切分结果:")
    for i, seg in ipairs(segments) do
        logger.info(string.format("  片段%d: 类型=%s, 内容='%s'", i, seg.type, seg.content))
    end

    return segments
end

-- 带日志记录和分隔符的版本
function text_splitter.split_and_convert_input_with_log_and_delimiter(input, logger, rawenglish_delimiter_before,
    rawenglish_delimiter_after, replace_punct_enabled)
    logger.info("开始处理输入: " .. input .. "，英文模式符号分隔符: '" ..
                    (rawenglish_delimiter_before or "") .. "' '" .. (rawenglish_delimiter_after or "") .. "'")
    logger.info("标点符号替换开关: " .. tostring(replace_punct_enabled or false))

    local segments = text_splitter.split_and_convert_input_with_delimiter(input, rawenglish_delimiter_before,
        rawenglish_delimiter_after, replace_punct_enabled)

    logger.info("切分结果:")
    for i, seg in ipairs(segments) do
        logger.info(string.format("  片段%d: 类型=%s, 内容='%s'", i, seg.type, seg.content))
    end

    return segments
end

-- 带日志记录的split_by_rawenglish函数
function text_splitter.split_by_rawenglish_with_log(input, seg_start, seg_end, delimiter_before, delimiter_after, logger)
    logger.info("开始使用split_by_rawenglish处理输入: " .. input .. "，分隔符: '" ..
                    (delimiter_before or "") .. "' '" .. (delimiter_after or "") .. "'")

    local segments = text_splitter.split_by_rawenglish(input, seg_start, seg_end, delimiter_before, delimiter_after)

    logger.info("split_by_rawenglish切分结果:")
    for i, seg in ipairs(segments) do
        logger.info(string.format("  片段%d: 类型=%s, 内容='%s'", i, seg.type, seg.content))
    end

    return segments
end

-- 搜索功能 - 跳过英文模式符号包围的部分, 我自己的版本
function text_splitter.find_text_skip_rawenglishs(input, search_str, start_pos, logger)

    --[[  1. 首先判断是否存在英文模式符号，如果不存在，就使用原来的搜索方式: local found_pos = string.find(confirmed_pos_input, add_search_move_str, search_start_pos, true)
    2. 首先找出字符串中所有英文模式符号包裹的范围, 记录下英文模式符号的索引范围.
    2.  如果存在英文模式符号, 首先使用string.find搜索第一个符合字符串位置, 得到光标位置, 判断是否处于英文模式符号范围当中, 如果处于英文模式符号当中, 则从搜索到的光标位置继续向后搜索. 
    3. 如果不处于英文模式符号当中,则返回对应索引值.
    ]]

    start_pos = start_pos or 1

    logger.info(string.format("开始搜索: 输入='%s', 搜索字符串='%s', 起始位置=%d", input, search_str,
        start_pos))

    -- 1. 首先判断是否存在英文模式符号，如果不存在，就使用原来的搜索方式
    if not string.find(input, text_splitter.english_mode_symbol) then
        logger.info("未发现英文模式符号，使用原来的搜索方式")
        local found_pos = string.find(input, search_str, start_pos, true)
        if found_pos then
            logger.info(string.format("找到匹配: 位置=%d", found_pos))
        else
            logger.info("未找到匹配")
        end
        return found_pos
    end

    -- 2&3. 如果存在英文模式符号，使用string.find搜索，但跳过英文模式符号区域
    local current_search_pos = start_pos

    while current_search_pos <= #input do
        -- 使用string.find搜索第一个符合字符串位置
        local found_pos = string.find(input, search_str, current_search_pos, true)

        if not found_pos then
            -- 没有找到匹配
            logger.info("未找到匹配")
            return nil
        end

        logger.info(string.format("string.find找到候选位置: %d", found_pos))

        -- 判断是否处于英文模式符号范围当中
        if not text_splitter.if_in_rawenglish(input, found_pos) then
            -- 如果不处于英文模式符号当中，返回对应索引值
            logger.info(string.format("找到有效匹配: 位置=%d", found_pos))
            return found_pos
        else
            -- 如果处于英文模式符号当中，则从搜索到的光标位置继续向后搜索
            logger.info(string.format("位置%d处于英文模式符号区域内，继续搜索", found_pos))
            current_search_pos = found_pos + 1
        end
    end

    logger.info("未找到匹配")
    return nil
end

-- 带循环搜索的版本 - 如果从指定位置没找到，从头开始搜索
function text_splitter.find_text_skip_rawenglishs_with_wrap(input, search_str, start_pos, logger)

    start_pos = start_pos or 1

    logger.info(string.format("开始循环搜索: 输入='%s', 搜索字符串='%s', 起始位置=%d", input,
        search_str, start_pos))

    -- 先从指定位置搜索
    local found_pos = text_splitter.find_text_skip_rawenglishs(input, search_str, start_pos, logger)

    if found_pos then
        return found_pos
    end

    -- 如果没找到且起始位置不是1，从头开始搜索
    if start_pos > 1 then
        logger.info("从指定位置未找到，从头开始搜索")
        return text_splitter.find_text_skip_rawenglishs(input, search_str, 1, logger)
    end

    return nil
end

-- 函数功能：给字符串，和索引值，然后判断索引值是否在英文模式符号范围之内，如果在，返回真，如果不在返回假
function text_splitter.if_in_rawenglish(input, pos)
    if not input or not pos or pos <= 0 or pos > #input then
        return false
    end

    -- 解析英文模式符号区域
    local rawenglish_regions = {}
    local in_rawenglish = false
    local rawenglish_start = nil
    local rawenglish_count = 0

    -- 统计英文模式符号数量和位置
    for i = 1, #input do
        if input:sub(i, i) == text_splitter.english_mode_symbol then
            rawenglish_count = rawenglish_count + 1
            if not in_rawenglish then
                -- 开始英文模式符号区域
                rawenglish_start = i
                in_rawenglish = true
            else
                -- 结束英文模式符号区域
                table.insert(rawenglish_regions, {
                    start = rawenglish_start,
                    _end = i
                })
                in_rawenglish = false
                rawenglish_start = nil
            end
        end
    end

    -- 如果有未配对的英文模式符号（奇数个），最后一个英文模式符号到末尾都跳过
    if rawenglish_count % 2 == 1 and rawenglish_start then
        table.insert(rawenglish_regions, {
            start = rawenglish_start,
            _end = #input
        })
    end

    -- 检查位置是否在英文模式符号区域内
    for _, region in ipairs(rawenglish_regions) do
        if pos >= region.start and pos <= region._end then
            return true
        end
    end

    return false
end

return text_splitter
