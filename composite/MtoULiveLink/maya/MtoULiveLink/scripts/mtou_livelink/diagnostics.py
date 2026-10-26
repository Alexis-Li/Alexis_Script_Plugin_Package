"""Stable diagnostic codes and the diagnostics derived from Unreal replies."""

from .config import WORKFLOW_MODEL

DIAGNOSTICS = {
    "PROTOCOL_VERSION_MISMATCH": (
        "Maya 与 UE 插件版本不匹配",
        "请安装同一版本的 Maya 和 UE MtoU_LiveLink 组件。"),
    "UNREAL_NOT_REACHABLE": (
        "无法连接 Unreal Editor",
        "请确认 UE 已启动、MtoU_LiveLink 已启用，并检查 54321 端口。"),
    "SECOND_CLIENT_REJECTED": (
        "Unreal 已连接另一个 Maya",
        "请先断开另一个 Maya 会话，再重新连接。"),
    "NO_BINDING_ACTOR": (
        "UE 关卡中没有 MtoU Binding Actor",
        "请在 UE 中将当前服装的 Binding 拖入关卡后重试。"),
    "MULTIPLE_BINDING_ACTORS": (
        "UE 关卡中有多个 MtoU Binding Actor",
        "请删除旧 Actor，仅保留当前服装的一个 Binding Actor。"),
    "INVALID_BINDING": (
        "UE Binding 资产无效",
        "请确认 Binding 已指定有效的 Skeletal Mesh。"),
    "SKELETON_MISMATCH": (
        "骨架与 Unreal Skeletal Mesh 不匹配",
        "请按 UE 详情检查首个未映射路径、父级与导入改名候选；"
        "确认资产确实不一致后再重新导出或导入。"),
    "BLENDSHAPE_MISMATCH": (
        "BlendShape 不完全匹配，连接可用但表情可能不完整",
        "请查看详情中 Maya 和 UE 各自缺少的名称，并确认 UE 服装是否正确。"),
    "NO_VISIBLE_SKINNED_MESH": (
        "没有找到可见的蒙皮网格",
        "请确认 Display 控制器已选择正确服装，并检查角色、头发和服装的显示状态。"),
    "INVALID_FRAME_RATE": (
        "Maya 场景帧率不受支持",
        "请将 Maya Time Unit 设置为 1–60 fps 范围内的动画帧率。"),
    "INVALID_MESSAGE": (
        "Maya 与 UE 的传输数据无效",
        "请确认两端插件版本一致；若仍失败，请复制诊断详情。"),
    "STREAM_INTERRUPTED": (
        "Live Link 连接已中断",
        "请确认 UE 仍在运行且 Binding Actor 有效，然后重新连接。"),
    "SAMPLING_FAILED": (
        "Maya 动画采样失败",
        "请检查骨架、BlendShape 和当前服装是否在连接期间被修改。"),
    "BIND_POSE_INVALID": (
        "无法读取可靠的 Maya 绑定姿势",
        "请检查绑定矩阵是否包含非有限值、不可逆矩阵，或存在无法可靠决胜的冲突。"),
    "INCOMPLETE_SKELETON": (
        "角色骨架存在未能发布的关节分支",
        "请检查详情中的首个缺失关节路径及原因；只有 joint 与通向关节的 transform"
        "才能发布，中间出现其他类型节点时请整理绑定后重试。"),
    "INTERNAL_ERROR": (
        "MtoU_LiveLink 发生内部错误",
        "请复制诊断详情并重新启动连接。"),
    "ROLE_SETUP_FAILED": (
        "角色设置未完成",
        "请按弹窗中的提示检查根骨骼和 Display 控制器。"),
    "DUPLICATE_BONE_NAMES": (
        "骨架中存在重名骨骼",
        "可以继续连接并由 UE 尝试映射；也可点击“选中重名骨骼”定位整理。"),
    "BONE_NAME_REMAP": (
        "UE 已自动映射重名骨骼",
        "连接可用；建议后续统一 Maya 与 UE 的骨骼名称，并保留本次映射详情。"),
    "PAIRING_REQUIRED": (
        "UE 尚未配对镜头联动目标",
        "请在 UE 详情面板中配对主角色（有副对象时也配对副对象），"
        "然后重新连接或进入镜头联动。"),
    "TARGET_UNKNOWN": (
        "UE 配对目标已不存在",
        "请确认 Binding Actor 仍在关卡中且未被删除或卸载，然后重新配对。"),
    "WRITER_NOT_ISOLATABLE": (
        "UE 目标的写入者无法安全隔离",
        "请按 UE 详情中的来源处理该轨道或动画后重新进入镜头联动；工具不会暂停整个镜头 Sequence。"),
    "SHARED_WRITER": (
        "UE 目标与其他对象共享写入者",
        "请解除共享轨道或改用独立目标，再重新进入镜头联动。"),
    "MOVING_SOCKET": (
        "UE 目标挂在会移动的 socket 上",
        "请把目标改为世界锚定的 Actor，避免父级运动被应用两次。"),
    "ANCHOR_MISMATCH": (
        "副对象与主角色在 UE 中的放置不一致",
        "请把副对象的绑定 Actor 放到与主角色相同的位置和朝向；"
        "联动期间不要移动任一目标。"),
    "OBJECT_SET_UNSUPPORTED": (
        "当前操作只支持单个角色",
        "缓存播放和模型工作流只支持主角色；请先在“对象配对”窗口中移除副对象。"),
    "MODE_CONFLICT": (
        "镜头联动模式冲突",
        "请等待当前转换结束，或先返回实时预览后重试。"),
    "CACHE_NOT_CLEARED": (
        "缓存播放尚未清除，无法进入镜头联动",
        "请先返回实时预览并等待缓存清除完成，再进入镜头联动。"),
    "SET_REVISION_STALE": (
        "UE 使用了过期的角色集合",
        "请重新连接以建立当前集合，再进入镜头联动。"),
    "SPACE_REVISION_STALE": (
        "UE 使用了过期的共同空间",
        "请重新建立会话与参考；仅修改版本号不能继续联动。"),
    "EVAL_STALE": (
        "UE 已取消该求值目标",
        "无需处理；工具会丢弃该结果并使用最新目标。"),
    "RESOURCE_LIMIT": (
        "镜头联动数据超出冻结的资源上限",
        "请缩短角色或减少 BlendShape 后重试；预算在测试前固定，不会为通过而放宽。"),
    "REFERENCE_STALE": (
        "场景参考属于旧的共同空间",
        "请在 UE 中重新导出当前范围的参考交接后再导入。"),
    "REFERENCE_DIGEST_MISMATCH": (
        "场景参考交接内容与摘要不符",
        "请确认交接目录未被修改，并在 UE 中重新导出。"),
    "REFERENCE_IMAGE_DATA": (
        "场景参考交接包含图片数据",
        "请在 UE 中关闭贴图烘焙后重新导出；工具不导入图片数据。"),
    "REFERENCE_OVER_BUDGET": (
        "场景参考超出冻结的规模上限",
        "请缩小 UE 参考范围或减少对象后再导出。"),
    "CONTAINER_NOT_OWNED": (
        "参考容器名称被场景中的其他节点占用",
        "请重命名该节点或删除它，然后重试导入；工具只删除自己创建的容器。"),
    "RECOVERY_REFUSED": (
        "上一次参考更新的恢复被拒绝",
        "请按诊断详情中的残留路径处理后重试；旧参考会保留在原地。"),
    "LINKAGE_NOT_ACTIVE": (
        "当前不在镜头联动状态",
        "请先进入镜头联动，再执行该操作。"),
    "CACHED_PLAYBACK_NOT_READY": (
        "缓存播放需要已连接的 Unreal",
        "请先完成角色设置并连接 Unreal，再进入缓存播放。"),
    "CACHED_PLAYBACK_NO_CACHE": (
        "没有可上传的缓存",
        "请先捕获当前 Maya 播放范围或自定义范围。"),
    "CACHED_PLAYBACK_SPACE": (
        "临时磁盘空间不足",
        "请缩短 Playback Range 或清理当前用户的临时磁盘空间后重试。"),
    "CACHED_PLAYBACK_CANCELLED": (
        "缓存捕获已取消",
        "已删除未完成缓存并恢复到实时预览。"),
    "CACHED_PLAYBACK_INCOMPATIBLE": (
        "缓存与当前角色快照不兼容",
        "请为当前角色和服装重新捕获缓存。"),
    "CACHED_UPLOAD_FAILED": (
        "缓存上传被 Unreal 拒绝",
        "Unreal 已丢弃未完成的缓存；请重新捕获后再试。"),
    "CACHED_PLAYBACK_FRAME_LIMIT": (
        "缓存帧数超过 20,000 上限，捕获已提前停止",
        "请缩短 Playback Range 后重新捕获。"),
    "CACHED_PLAYBACK_PAYLOAD_LIMIT": (
        "缓存编码体积超过 1 GiB 上限，捕获已提前停止",
        "请缩短 Playback Range 或减少 BlendShape 数量后重新捕获。"),
    "CACHED_PLAYBACK_PERFORMANCE": (
        "缓存回放跟不上捕获帧率，已停止",
        "已完整保留缓存；请关闭占用性能的程序后在 UE 中再次播放。"),
    "CACHED_PLAYBACK_CAPTURE_RANGE": (
        "捕获范围无效",
        "请输入整数开始和结束帧，结束帧不得早于开始帧。"),
    "PREVIEW_NOT_READY": (
        "模型预览尚未生成，无法连接模型工作流",
        "请先在 UE 中对该 Binding Actor 执行 Refresh Preview 生成预览，再重新连接。"),
    "PREVIEW_BUILD_FAILED": (
        "模型预览生成失败",
        "请按诊断详情修复输入后，在 UE 中重新执行 Refresh Preview。"),
    "PREVIEW_MORPH_MISMATCH": (
        "Maya 与模型预览的 BlendShape 没有交集",
        "请确认当前服装与 UE Binding 一致并重新生成预览；也可关闭“传递 BS”进行仅骨骼对比。"),
}


def make_diagnostic(code, message="", solution="", details=""):
    summary, default_solution = DIAGNOSTICS.get(code, DIAGNOSTICS["INTERNAL_ERROR"])
    return {
        "code": code,
        "summary": summary,
        "solution": solution or default_solution,
        "message": message or summary,
        "details": details or message,
    }


def blendshape_warning_from_reply(reply):
    missing_in_unreal = list(reply.get("missing_in_unreal")
                             or reply.get("missing_curves") or [])
    missing_in_maya = list(reply.get("missing_in_maya") or [])
    bone_name_remaps = list(reply.get("bone_name_remaps") or [])
    return {
        "missing_in_unreal": missing_in_unreal,
        "missing_in_maya": missing_in_maya,
        "bone_name_remaps": bone_name_remaps,
        "has_warning": bool(missing_in_unreal or missing_in_maya or bone_name_remaps),
    }


def filter_bone_driven_differences(init_message, reply):
    """Drop UE-only Morph differences for outfits that declare no BlendShapes.

    A bone-driven outfit declares zero BlendShape names, so generated Morph
    Targets missing in Maya are expected and are not an expression-coverage
    warning.
    """
    if (init_message.get("workflow") == WORKFLOW_MODEL
            and init_message.get("blendshapes_enabled")
            and not init_message.get("curves")):
        reply = dict(reply)
        reply["missing_in_maya"] = []
    return reply
