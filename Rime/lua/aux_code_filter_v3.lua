-- 对双拼进行形码的输入
-- 参考: https://github.com/HowcanoeWang/rime-lua-aux-code
-- 升级版本的辅助码匹配,之前只想到匹配第一个字,或者最后一个字,但是发现可以全部都匹配,候选词当中的所有字都进行匹配, 把匹配上的放到前边来.
-- v3升级, 考虑input当中有标点符号的情况.计算计算奇数个和偶数个的时候,删掉所有标点符号.
-- 最后三个字母匹配, 过滤掉长度超过的
-- 候选项当中, 和所有候选项，进行匹配，如果匹配上辅助码的就放到前边来
local logger_module = require("logger")
local debug_utils = require("debug_utils")

-- 创建当前模块的日志记录器
local logger = logger_module.create("aux_code_filter_v3", {
    enabled = true, -- 启用日志以便测试
    unique_file_log = false, -- 启用日志以便测试
    log_level = "DEBUG"
})
-- 清空日志文件
logger.clear()

local aux_code_filter = {}
local last_segment_input = ""

-- 模块级配置缓存
aux_code_filter.single_fuzhu = false
aux_code_filter.fuzhu_mode = ""
aux_code_filter.shuangpin_zrm_txt = ""
aux_code_filter.aux_hanzi_code = {}
aux_code_filter.aux_code_hanzi = {}

-- 配置更新函数
function aux_code_filter.update_current_config(config)
    logger.debug("开始更新aux_code_filter_v3模块配置")

    aux_code_filter.single_fuzhu = config:get_bool("aux_code/single_fuzhu") or false
    aux_code_filter.fuzhu_mode = config:get_string("aux_code/fuzhu_mode") or ""
    aux_code_filter.shuangpin_zrm_txt = config:get_string("aux_code/shuangpin_zrm_txt") or ""
    aux_code_filter.english_mode_symbol = config:get_string("translator/english_mode_symbol") or ""

    logger.debug("single_fuzhu: " .. tostring(aux_code_filter.single_fuzhu))
    logger.debug("fuzhu_mode: " .. aux_code_filter.fuzhu_mode)
    logger.debug("shuangpin_zrm_txt: " .. aux_code_filter.shuangpin_zrm_txt)

    -- 重新加载辅助码数据
    aux_code_filter.aux_hanzi_code, aux_code_filter.aux_code_hanzi = aux_code_filter.readAuxTxt(
        aux_code_filter.shuangpin_zrm_txt)

    logger.debug("aux_code_filter_v3模块配置更新完成")
end

function aux_code_filter.init(env)

    logger.debug("aux_code_filter_v3 init")
    logger.debug("=" .. string.rep("=", 60))

    local engine = env.engine
    local context = engine.context
    local config = engine.schema.config
    aux_code_filter.update_current_config(config)

    ----------------------------
    -- 每一次选词上屏, 判断aux_code_filter.set_fuzhuma的值, 如果存在辅助码就把辅助码删除掉 --
    -- 原来这就是通知消息存在的意义,因为选词之后要进行一些处理,将最后一个辅助码删除掉
    ----------------------------
    env.select_notifier = engine.context.select_notifier:connect(function(context)
        logger.debug("aux_code_filter.set_fuzhuma: '" .. tostring(aux_code_filter.set_fuzhuma))
        -- 如果是单个的标点符号,则应该直接上屏
        local input = context.input
        -- 检查删除辅助码后的情况
        local segmentation = context.composition:toSegmentation()

        local segmentation_input = segmentation.input
        logger.debug("segmentation.input: '" .. segmentation.input)
        local confirmed_position = segmentation:get_confirmed_position()
        local current_start = segmentation:get_current_start_position()
        local current_end = segmentation:get_current_end_position()
        local confirmed_position = segmentation:get_confirmed_position()
        -- 这个不是被选词确认的而是被多个segment前边的顶上去的那种,如果顶上去那种不考虑的话,可以用这个
        local segmente_input = input:sub(current_start + 1, current_end)

        if #segmente_input == 1 and segmente_input:match("[,.!?;:()%[%]<>/_=+*&^%%$#@~|%-'\"]") then
            -- 如果剩下的是英文标点符号，则直接上屏
            logger.debug("剩余为英文标点符号，直接上屏: " .. segmente_input)
            -- context:commit()
            context:confirm_current_selection()
            return
        end

        -- 再加一个功能：如果第一个 seg 中是一个标点符号的话，应该直接上屏这个标点符号。
        -- logger.debug("select_notifier函数: 选词通知打印segmentation: ")
        -- debug_utils.print_segmentation_info(segmentation, logger)

        -- 应该是如何处理了辅助码需要进行
        if not aux_code_filter.set_fuzhuma then
            return
        end

        -- 添加错误捕获
        local success, error_msg = pcall(function()
            -- 尝试删除辅助码, 但是如果光标不在最后一位, 应该删除光标位置前边的最后一个字母
            local input = context.input
            logger.debug("select_notifier函数: 选词上屏,输入文本: " .. input)

            -- 删除最后一个辅助码
            context:pop_input(1)
            aux_code_filter.set_fuzhuma = false -- 重置标志位

            input = context.input
            logger.debug("删除辅助码后input: " .. input)

            -- 检查删除辅助码后的情况
            local segmentation = context.composition:toSegmentation()
            local confirmed_position = segmentation:get_confirmed_position()
            local unconfirmed_length = #input - confirmed_position

            logger.debug("confirmed_position=" .. confirmed_position .. ", unconfirmed_length=" .. unconfirmed_length)

            -- 当没有未确认的字符时，直接上屏
            if unconfirmed_length == 0 then
                logger.debug("没有剩余未确认字符,直接上屏")
                context:commit()
            end

        end)

        if not success then
            logger.error("选词上屏处理过程中发生错误: " .. tostring(error_msg))
            -- 重置标志位确保不影响后续操作
            aux_code_filter.set_fuzhuma = false
        end
    end)

end

----------------
-- 阅读辅码文件, 功能是将字典文件读取到缓存当中aux_code_filter.cache
-- 貌似不需要更改 --
----------------
function aux_code_filter.readAuxTxt(txtpath)
    if aux_code_filter.cache and aux_code_filter.cache_aux_code_hanzi then
        logger.debug("aux_code_filter有缓存")
        return aux_code_filter.cache, aux_code_filter.cache_aux_code_hanzi
    end

    -- local defaultFile = '20250612_phrases_shuangpin_org.txt'
    local userPath = rime_api.get_user_data_dir() .. "/lua/aux_code/"
    local fileAbsolutePath = userPath .. txtpath .. ".txt"
    logger.debug("aux_code_filter读取文件: " .. fileAbsolutePath)

    local file = io.open(fileAbsolutePath, "r")
    if not file then
        logger.error("不能打开辅助码文件")
        return {}
    end

    local aux_hanzi_code = {}
    local aux_code_hanzi = {}
    for line in file:lines() do
        line = line:match("[^\r\n]+") -- 去掉換行符，不然 value 是帶著 \n 的
        local key, value = line:match("([^=]+)=(.+)") -- 分割 = 左右的變數
        if key and value then
            aux_hanzi_code[key] = aux_hanzi_code[key] or {}
            table.insert(aux_hanzi_code[key], value)
            -- 将后面的字母作为key, 汉字作为value
            aux_code_hanzi[value] = aux_code_hanzi[value] or {}
            table.insert(aux_code_hanzi[value], key)
        end
    end
    file:close()
    -- 確認 code 能打印出來
    -- for key, value in pairs(aux_code_filter.aux_code) do
    --     log.info(key, table.concat(value, ','))
    -- end

    aux_code_filter.cache = aux_hanzi_code
    aux_code_filter.cache_aux_code_hanzi = aux_code_hanzi
    logger.debug("aux_code_filter.cache读取成功")
    return aux_code_filter.cache, aux_code_filter.cache_aux_code_hanzi
end

-- 提取出当前候选词的中文, 和输入的匹配码, 返回是否匹配成功? 
local function fuzhuma_match(match_char, match_code)
    -- 进行辅助码匹配
    -- logger.debug("匹配字符: " .. match_char)
    local fuzhuma = aux_code_filter.aux_hanzi_code[match_char] -- 找到字符的辅助码
    if fuzhuma then -- 辅助码存在, 这个字是存在辅助码的
        for _, code in ipairs(fuzhuma) do -- 对这个字符的所有辅助码进行遍历,为这个一个字符可能有多个不同的辅助码
            if code:sub(3, 3) == match_code then -- 如果辅助码和输入的最后一个字母相同,则匹配成功
                logger.debug("匹配成功: match_char: " .. match_char .. "  match_code: " .. match_code)
                return 1
            elseif code:sub(3, 3) == "" then
                return 2
            end
        end
    end

    return
end

------------------
-- filter 主函数 --
------------------
function aux_code_filter.func(translation, env)
    logger.debug("aux_code_filter func")
    local context = env.engine.context
    local input = context.input

    -- 光标左侧的input就是segmentation_input, 去掉已经选词的部分,就是confirmed_pos_segmentation_input
    local segmentation = env.engine.context.composition:toSegmentation()
    local segmentation_input = segmentation.input
    logger.debug("segmentation.input: '" .. segmentation.input)
    local confirmed_position = segmentation:get_confirmed_position()
    local current_start = segmentation:get_current_start_position()
    local current_end = segmentation:get_current_end_position()
    local confirmed_position = segmentation:get_confirmed_position()

    -- 这个不是被选词确认的而是被多个segment前边的顶上去的那种,如果顶上去那种不考虑的话,可以用这个
    local segmente_input = input:sub(current_start + 1, current_end)

    local confirmed_pos_segmentation_input = segmentation_input:sub(confirmed_position + 1)
    logger.debug("confirmed_pos_segmentation_input: " .. confirmed_pos_segmentation_input)

    logger.debug("")
    logger.debug("=== 开始分析lua/aux_code_filter.lua ===")

    -- 豁免ai对话中的标签为"ai_talk"的内容部分

    -- 如果是在反引号模式中, 也不进入, 如果input长度小于3 或者是偶数,也不进入
    -- 如果是剩余的segmente_input小于3,还进不进入呢？按说也应该不进入, 只是我需要在选词之后, 保持set_fuzhuma为真
    -- `haha`w 这个时候,也是反引号模式,应该直接进入下面这个分支, 但要区分 hahaw
    -- 关键是之前设置,如果选词之后只剩一个字母,那么应该删除这个字母,怎么办呢?选词之后,也是只剩一个字母
    logger.debug("rawenglish_prompt: " .. context:get_property("rawenglish_prompt"))
    logger.debug("aux_code_filter.single_fuzhu: " .. tostring(aux_code_filter.single_fuzhu))
    logger.debug("#input长度: " .. tostring(#input))
    if not aux_code_filter.single_fuzhu or #input <= 2 or context:get_property("rawenglish_prompt") == "1" then
        logger.debug("当前输入#segmente_input长度小于等于2, set_fuzhuma设置为false")
        aux_code_filter.set_fuzhuma = false
        for cand in translation:iter() do
            yield(cand)
        end
        return
    end

    -- 候选词类型如果是自定义的那几种,应该直接跳过

    --[[ 这里有几种可能性: 1. sum:nihkwodema, 全部未确认, 应该切除前边算tags为"ai_talk"部分
    2. sum:nihkwodema, 确认到ni, 什么都不用做.
    3. sum: 应该切除前边算tags为"ai_talk"部分
    总的来说就是把前边这段排除掉。 所以三种可能性都可以把前边的tags部分先去掉,然后剩余的再从光标确认位置切割 ]]

    -- 检查第一个段落是否包含ai_talk标签
    if current_start == 0 then
        -- 那么需要调整segmente_input
        local first_segment = segmentation:get_at(0)
        if first_segment:has_tag("ai_talk") then
            local ai_segment_length = first_segment._end - first_segment.start
            logger.debug("发现AI段落，长度: " .. ai_segment_length .. "，内容: " ..
                             input:sub(first_segment.start + 1, first_segment._end))

            -- sum:nihk 长度7  ai_segment_length:长度3, 在这里应该已经去除掉前边的 sum: 了
            if ai_segment_length < #segmente_input then
                -- 重新计算segmente_input，排除AI段落, 在这里confirmed_position== 0, 所以从前边切除就可以
                segmente_input = input:sub(first_segment.length + 1, current_end)
                logger.debug("排除AI段落后的segmente_input: " .. segmente_input)
                -- sum:
            elseif ai_segment_length >= #segmente_input then
                -- AI段落占据了全部或大部分内容，跳过辅助码处理
                logger.debug("AI段落占据全部内容，跳过辅助码处理")
                aux_code_filter.set_fuzhuma = false
                for cand in translation:iter() do
                    yield(cand)
                end
                return
            end
        elseif first_segment:has_tag("ai_reply") then
            -- ai回复内容, 直接豁免处理
            aux_code_filter.set_fuzhuma = false
            for cand in translation:iter() do
                yield(cand)
            end
            return
        else
            logger.debug("未发现AI段落，使用原始segmente_input: " .. segmente_input)
        end
    else
        -- 不需要调整segmente_input
    end

    -- 这里对于标点符号的处理存在严重bug, 如果是选词之后剩余一个标点符号,不应该不处理.
    -- 所以对于#segmente_input == 1应该有三种情况,1.是辅助码,应该删除 2.是普通字母,这种就直接当做是辅助码了.

    -- 当#segmente_input为偶数进不来, 只有1能进来, 这时候也就是剩余一个辅助码, 但是在触发选词通知回调函数之前还会运行两次这个代码
    -- 如果在一次选词之后剩余一个字母,那么这个字母是辅助码，马上准备要删掉,也没什么作用.
    if #segmente_input == 1 then
        -- 分成两种情况1. 直接input就是segmente_input, 没有选词过, 则三个字母, 选择了前两个字母, 保留set_fuzhuma的值, 会删除辅助码,然后上屏
        -- 情况2: 多个字,选择了一部分, 如果5个字选择了4个,那么剩余3个,不会进入这个分支, set_fuzhuma原来是true, 选词会出发删除一个辅助码,剩余两个字符.
        -- 情况3: 如果5个字选择了5个字, 进入这个分支,保留set_fuzhuma的值, 会删除辅助码,然后上屏

        logger.debug("剩余#segmente_input == 1, 什么都不做直接返回, 保持set_fuzhuma : " ..
                         tostring(aux_code_filter.set_fuzhuma))

        for cand in translation:iter() do
            yield(cand)
        end
        return
    end
    local success, error_msg = pcall(function()

        local has_rawenglish = segmente_input:match(aux_code_filter.english_mode_symbol) ~= nil
        if has_rawenglish then
            -- 将segmente_input中的英文模式符号包裹的片段删除

            -- 首先检查倒数第二个字符是否为英文模式符号（在删除成对符号之前）
            local pattern =
                aux_code_filter.english_mode_symbol .. "[^" .. aux_code_filter.english_mode_symbol .. "]*" ..
                    aux_code_filter.english_mode_symbol

            -- 检查是否存在成对的英文模式符号
            if segmente_input:match(pattern) then
                -- 在删除成对符号之前，检查倒数第二个字符
                if #segmente_input >= 2 and segmente_input:sub(-2, -2) == aux_code_filter.english_mode_symbol then
                    logger.debug("倒数第二个字符是英文模式符号，直接返回false")
                    return false
                end

                -- 删除成对的英文模式符号
                segmente_input = segmente_input:gsub(pattern, "")
            end

            logger.debug("删除英文模式符号包裹片段后的segmente_input: " .. segmente_input)
            -- -- 如果删除英文模式符号片段之后,只剩下一个字母, 不应该触发删除辅助码
            -- aux_code_filter.set_fuzhuma = false

            -- 然后处理未配对的英文模式符号（从最后一个英文模式符号开始到末尾）
            local last_symbol_pos = segmente_input:match(".*" .. aux_code_filter.english_mode_symbol .. "()")
            if last_symbol_pos then
                -- 如果还有未配对的英文模式符号，移除从该位置开始的所有内容
                segmente_input = segmente_input:sub(1, last_symbol_pos - 2)
            end

        end

        -- 检查输入是否包含标点符号, 
        --[[ bug处理: 这里要考虑和ai对话标识符的冲突, 当标识符为 sum:  
    last_three_has_punctuation = um: 
    segmente_input = sum, 
    对了我根本不应该考虑这些，我应该考虑的事，是不是存在标签，如果存在标签，一切豁免。 ]]
        local last_three_has_punctuation = false
        local has_punctuation = segmente_input:match("[,.!?;:()%[%]<>/_=+*&^%%$#@~|%-'\"'`]") ~= nil
        if has_punctuation then
            logger.debug("有标点符号")
            last_three_has_punctuation = segmente_input:sub(-3):match("[,.!?;:()%[%]<>/_=+*&^%%$#@~|%-'\"'`]") ~= nil
            -- 删除segmente_input中的所有标点符号
            segmente_input = segmente_input:gsub("[,.!?;:()%[%]<>/_=+*&^%%$#@~|%-'\"']", "")
            logger.debug("删除标点符号后的segmente_input: " .. segmente_input)
        else
            -- 没有标点符号, 那就是正常长句, 对于这种和原来的处理方案一样
        end

        -- 重新检查删除标点符号后的长度
        if #segmente_input % 2 == 0 or #segmente_input == 1 then
            logger.debug("segmente_input长度是偶数或者长度为1,直接返回")
            aux_code_filter.set_fuzhuma = false
            for cand in translation:iter() do
                yield(cand)
            end
            return

        else
            -- 这个分支是删除标点符号之后,是奇数个字母,应该进行辅助码匹配.

        end

        -- local last_three_has_punctuation = segmente_input:sub(-3):match("[,.!?;:()%[%]<>/_=+*&^%%$#@~|%-`'\"']") ~= nil

        debug_utils.print_segmentation_info(segmentation, logger)
        -- debug_utils.print_candidate_info(cand, logger)

        if last_three_has_punctuation then
            -- 如果最后三位有标点符号,直接输出默认数据, 但我还是希望将超长度的过滤掉
            return false
        end

        logger.debug("开始辅助码匹配,输入文本: " .. segmente_input)
        -- 最后一个辅助码
        local last_code = segmente_input:sub(-1)
        logger.debug("last_char: " .. last_code)
        -- local auxCodes = aux_code_filter.aux_code[aux_chat] 
        -- 更新逻辑：没有匹配上就不出现再候选框里，提升性能
        local insert_second = {} -- 没有辅助码的优先字
        local insert_last = {}

        -- 20250730 修改不再单独处理#segmente_input == 3的情况,全部统一处理.
        logger.debug("aux_code_filter.fuzhu_mode: " .. aux_code_filter.fuzhu_mode)
        if aux_code_filter.fuzhu_mode == "single" then
            logger.debug("进入只匹配前三个分支, 直接返回true")
            return true
        end

        -- 标记使用了辅助码, 标记这个就是在选词之后删除辅助码, 如果辅助码已经包含进去了,就不用标记了.
        logger.debug("set_fuzhuma 设置为true")
        aux_code_filter.set_fuzhuma = true

        -- all模式, 对候选词中的所有字都进行匹配,只要匹配上了就输出,问题是从第一个开始,还是最后一个开始
        if aux_code_filter.fuzhu_mode == "all" then

            logger.debug("当前输入是奇数个, 开始辅助码匹配候选词中所有字模式模式")
            -- 开始对所有候选项进行遍历
            local count = 0

            -- 创建按匹配位置分组的候选词列表
            local matched_by_position = {} -- matched_by_position[1] 存储第一个字符匹配的候选词

            -- 最后一个辅助码替换完成, 只替换一个选项
            local last_replace_flag = false
            local first_preedit
            -- 对所有候选词进行遍历
            for cand in translation:iter() do
                count = count + 1

                -- 改成计算segment的覆盖范围有没有到最后一个字符
                local left_position = current_end - cand._end
                -- if count == 1 then
                --     debug_utils.print_candidate_info(cand, count, logger)

                --     logger.debug("current_end: " .. current_end .. " cand._end: " .. cand._end)
                --     logger.debug("left_position = current_end - cand._end: " .. left_position)
                -- end
                -- current_end 当前片段结束位置, cand._end 候选词结束位置, left_position剩余没有匹配到的input字符的位置

                if left_position == 0 then
                    -- 这些就是最后一个字母参与到组词的数据, 在这里应该有原生的preedit,我直接获取到就可以了
                    -- 直接获取到preedit, 然后将preedit中的最后一个音节替换为last_code, 然后将替换后的内容作为新的preedit
                    if not first_preedit then
                        first_preedit = cand.preedit or ""
                        -- logger.debug("first_preedit: " .. first_preedit)
                        -- 去掉收尾空格并删除最后一个音节（以空格分隔）
                        local trimmed = first_preedit:gsub("%s+$", "")
                        local without_last = trimmed:match("^(.*)%s+[^%s]+$")
                        first_preedit = without_last or ""
                        logger.debug("first_preedit去除最后一个音节: " .. first_preedit)
                    end

                elseif left_position == 1 then
                    -- 这些就是最后一个字母没有参与到组词的, 但是其他字母全部匹配的数据
                    -- 但是这个可能有很多个, 所以不能全部处理, 处理一个就可以了, 所以本分支无论如何只进入一次.
                    -- 当处理的时候有几种可能？有可能找到辅助码替换,有可能没有找到辅助码替换, 找到的,剩余再找到的就直接放到later里面.
                    -- 没找到的呢? 就将所有都放到later里面

                    -- 1. 对于第一个候选项直接放弃,因为这个候选词会带有最后一个字的长度出来的,但是也可能出来多个啊
                    -- 对于第2个候选项,直接进行替换最后一个字符
                    -- 1. 第一个候选词不对，这个候选词已经包含了最后一个字，并且进行了组合，我们要得是第二个候选项
                    -- 1. 首先获取当前候选词内容，切片提取最后一个字对应的字母
                    -- 2. 这两个字母,拼接上最后一个字母,合并成三个字母,到字典当中查找出对应的汉字
                    -- 2. 将这个汉字替换到候选词的最后一个字上面
                    -- 4. 如果没有找到这个汉字的话, 则保留最后一个汉字

                    if not last_replace_flag then
                        -- 只替换第一个, 然后last_replace_flag改成true,这个分支后面的就不处理了
                        last_replace_flag = true

                        -- 提取最后三个字符 
                        local cand_text = cand.text
                        logger.debug("cand_text: " .. cand_text)
                        local last_three_code = segmente_input:sub(-3)
                        logger.debug("last_three_code: " .. last_three_code)
                        -- 从字典中查找对应的文字
                        local chinese_char_list = aux_code_filter.aux_code_hanzi[last_three_code]

                        if chinese_char_list and #chinese_char_list > 0 then
                            -- logger.debug("set_fuzhuma 设置为true")
                            -- aux_code_filter.set_fuzhuma = true
                            -- 获取最后一个字符的位置
                            local last_char_index = utf8.offset(cand_text, -1)

                            -- 获取除了最后一个字符之外的部分
                            local text_without_last = cand_text:sub(1, last_char_index - 1)

                            -- 对每个匹配的汉字都生成一个候选项
                            for i, chinese_char in ipairs(chinese_char_list) do
                                logger.debug("第" .. i .. "个匹配的汉字: " .. chinese_char)
                                -- 拼接新的文本
                                local new_text = text_without_last .. chinese_char
                                -- 创建新的候选词
                                local new_cand = Candidate(cand.type, cand.start, cand._end, new_text, cand.comment)
                                -- 向后扩展一位,将辅助码也包含进来
                                logger.debug("cand.start: " .. cand.start)
                                logger.debug("cand._end: " .. cand._end)
                                logger.debug("new_text: " .. new_text)
                                logger.debug("cand.preedit: " .. cand.preedit)
                                -- new_cand.preedit = cand.preedit
                                if not first_preedit then
                                    -- 如果没有生成这个first_preedit,就用当前候选词的preedit
                                    first_preedit = cand.preedit
                                end
                                new_cand.preedit = first_preedit

                                yield(new_cand)
                            end

                        else
                            logger.debug("最后三个字符未查找到匹配的汉字")
                            if not first_preedit then
                                -- 如果没有生成这个first_preedit,就用当前候选词的preedit
                                first_preedit = cand.preedit
                            end
                            cand.preedit = first_preedit
                            yield(cand)
                            -- table.insert(insert_last, cand)
                        end

                    else
                        table.insert(insert_last, cand)
                    end

                else
                    -- 剩余的长度不足以覆盖全部输入的候选项, 从匹配到字符的顺序进行依次排列
                    local cand_text = cand.text
                    local matched_position = 0
                    local count_char = 0
                    local match_flag = false
                    -- 遍历候选词中的每个字符
                    for pos, code in utf8.codes(cand_text) do
                        count_char = count_char + 1
                        local char = utf8.char(code)

                        -- 这个地方忘记改了, 有可能返回1,返回2,1就是匹配成功,2是匹配到常用字上面了
                        if fuzhuma_match(char, last_code) == 1 then
                            matched_position = count_char
                            break -- 只要有一个字符匹配就可以了
                            -- elseif fuzhuma_match(char, last_code) == 2 then
                            --     -- 要不要匹配常用字呢？不用了把
                        end
                    end

                    if matched_position == 0 then
                        -- 没有匹配
                        table.insert(insert_last, cand)
                    else
                        -- 有匹配，按位置存储
                        if not matched_by_position[matched_position] then
                            matched_by_position[matched_position] = {}
                        end
                        table.insert(matched_by_position[matched_position], cand)
                    end
                end
            end

            -- 按照匹配位置从前到后输出候选词
            -- 获取所有匹配位置并排序
            local positions = {}
            for pos, _ in pairs(matched_by_position) do
                table.insert(positions, pos)
            end
            table.sort(positions)
            logger.debug("匹配候选词并排序成功,现在开始输出候选词")
            -- 按位置顺序输出（第1个字符匹配的、第2个字符匹配的、第3个字符匹配的...）
            for _, pos in ipairs(positions) do
                logger.debug("输出第" .. pos .. "个字符匹配的候选词")
                for _, cand in ipairs(matched_by_position[pos]) do
                    logger.debug("匹配成功的候选词" .. cand.text .. " 匹配位置: " .. tostring(pos))
                    yield(cand)
                end
            end

            -- 把沒有匹配上的候选项添加上
            for _, cand in ipairs(insert_last) do
                yield(cand)
            end

            return true
        elseif aux_code_filter.fuzhu_mode == "before" then

            logger.debug("当前输入是奇数个, 开始辅助码匹配候选词中所有字模式模式")
            -- 开始对所有候选项进行遍历
            local count = 0

            -- 创建按匹配位置分组的候选词列表
            local matched_by_position = {} -- matched_by_position[1] 存储第一个字符匹配的候选词

            -- 最后一个辅助码替换完成, 只替换一个选项
            local first_left_cand = false
            local first_preedit
            -- 对所有候选词进行遍历

            if #input == 3 then

                for cand in translation:iter() do
                    local cand_text = cand.text
                    if utf8.len(cand_text) == 1 then
                        -- 单个码点
                        if fuzhuma_match(cand_text, last_code) == 1 then
                            yield(cand)
                        elseif fuzhuma_match(cand_text, last_code) == 2 then
                            table.insert(insert_second, cand)
                        else
                            table.insert(insert_last, cand)
                        end
                    end
                end
                -- 循环完成之后，依次输出剩余的insert_second和insert_last
                for _, cand in ipairs(insert_second) do
                    yield(cand)
                end

                for _, cand in ipairs(insert_last) do
                    yield(cand)
                end

            else -- 不仅仅是三个字母的时候，走这个分支。
                for cand in translation:iter() do
                    count = count + 1
                    local left_position = current_end - cand._end
                    if left_position == 0 then
                        -- 这种候选词是长度超标的，直接放弃
                        -- 这些就是最后一个字母参与到组词的数据, 在这里应该有原生的preedit,我直接获取到就可以了
                        -- 直接获取到preedit, 然后将preedit中的最后一个音节替换为last_code, 然后将替换后的内容作为新的preedit
                        if not first_preedit then
                            first_preedit = cand.preedit or ""
                            -- logger.debug("first_preedit: " .. first_preedit)
                            -- 去掉收尾空格并删除最后一个音节（以空格分隔）
                            local trimmed = first_preedit:gsub("%s+$", "")
                            local without_last = trimmed:match("^(.*)%s+[^%s]+$")
                            first_preedit = without_last or ""
                            logger.debug("first_preedit去除最后一个音节: " .. first_preedit)
                        end
                    else
                        -- 剩余的长度不足以覆盖全部输入的候选项, 从匹配到字符的顺序进行依次排列
                        local cand_text = cand.text
                        local matched_position = 0
                        local count_char = 0
                        local match_flag = false
                        -- 这种候选词的第一个可以保留，只保留一个就可以
                        if not first_left_cand then -- 如果还没有则保留一个，保留应该直接yeild
                            
                            if not first_preedit then
                                -- 如果没有生成这个first_preedit,就用当前候选词的preedit
                                first_preedit = cand.preedit
                            end
                            -- first_preedit = first_preedit .. last_code
                            first_left_cand = true
                            cand.preedit = first_preedit .. " " .. last_code
                            cand._end = cand._end + 1
                            yield(cand)
                        else

                            -- 遍历候选词中的每个字符, 也就是如果有多个字, 一个字一个字的匹配
                            for pos, code in utf8.codes(cand_text) do
                                count_char = count_char + 1
                                local char = utf8.char(code)

                                -- 这个地方忘记改了, 有可能返回1,返回2,1就是匹配成功,2是匹配到常用字上面了
                                if fuzhuma_match(char, last_code) == 1 then
                                    matched_position = count_char
                                    break -- 只要有一个字符匹配就可以了
                                    -- elseif fuzhuma_match(char, last_code) == 2 then
                                    --     --  匹配常用字,没有辅助码的优先字
                                    --     matched_position = count_char
                                end
                            end

                            if matched_position == 0 then
                                -- 没有匹配
                                table.insert(insert_last, cand)
                            else
                                -- 有匹配，按位置存储
                                if not matched_by_position[matched_position] then
                                    matched_by_position[matched_position] = {}
                                end
                                table.insert(matched_by_position[matched_position], cand)
                            end
                        end

                    end

                end

                -- 按照匹配位置从前到后输出候选词
                -- 获取所有匹配位置并排序
                local positions = {}
                for pos, _ in pairs(matched_by_position) do
                    table.insert(positions, pos)
                end
                table.sort(positions)
                logger.debug("匹配候选词并排序成功,现在开始输出候选词")
                -- 按位置顺序输出（第1个字符匹配的、第2个字符匹配的、第3个字符匹配的...）
                for _, pos in ipairs(positions) do
                    logger.debug("输出第" .. pos .. "个字符匹配的候选词")
                    for _, cand in ipairs(matched_by_position[pos]) do
                        logger.debug("匹配成功的候选词" .. cand.text .. " 匹配位置: " .. tostring(pos))
                        yield(cand)
                    end
                end

                -- 把沒有匹配上的候选项添加上
                for _, cand in ipairs(insert_last) do
                    yield(cand)
                end
            end

            return true
        elseif aux_code_filter.fuzhu_mode == "after" then
            logger.debug("当前输入是奇数个, 开始辅助码匹配最后一个字模式模式")
            -- 开始对所有候选项进行遍历
            local count = 0
            for cand in translation:iter() do
                count = count + 1

                -- 提取第一个字符
                local the_char = ""
                local cand_text = cand.text

                -- 提取最后一个字符
                -- logger.debug("cand_text: " .. cand_text)
                local index = utf8.offset(cand_text, -1)
                the_char = cand_text:sub(index)

                if count == 1 then
                    -- 第一个候选项直接输出，不进行辅助码匹配
                    yield(cand)
                elseif fuzhuma_match(the_char, last_code) then
                    yield(cand)
                else
                    table.insert(insert_last, cand)
                end

            end
            -- 把沒有匹配上的待選給添加上
            for _, cand in ipairs(insert_last) do
                yield(cand)
            end

            return true
        end

    end)

    if not success then
        logger.error("aux_code_filter.func 执行过程中发生错误: " .. tostring(error_msg))
        -- 发生错误时回退到显示所有候选项，确保输入法正常工作
        for cand in translation:iter() do
            -- 过滤掉长度匹配到最后一个辅助码的
            local left_position = current_end - cand._end
            if left_position ~= 0 then
                yield(cand)
            else

            end
        end
    elseif error_msg == false then
        logger.debug("直接返回了false")
        -- 没有进入到辅助码合适的字符, 直接输出原来的候选词
        for cand in translation:iter() do
            logger.debug("cand.text: " .. cand.text)
            yield(cand)
            -- -- 过滤掉长度匹配到最后一个辅助码的
            -- local left_position = current_end - cand._end
            -- if left_position ~= 0 then
            --     yield(cand)
            -- else

            -- end

        end
    end

end

function aux_code_filter.fini(env)
    if env.select_notifier then
        env.select_notifier:disconnect()
    end
end

return aux_code_filter

