// 审批回路：ask → 挂起 → 问 → 恢复。
//
// 设计上的两个决定：
//
// 1. 非交互环境下 ask 等价于 deny，而不是 allow。
//    没人能回答的时候放行，等于这条规则从来没存在过。
//
// 2. 被拒绝的结果要「喂回模型」，不是抛异常。
//    模型需要知道"这条路被用户堵死了"，才会去想别的办法；
//    抛异常只会让整轮失败，用户还得从头再来。
export interface ApprovalRequest {
  id: string;
  tool: string;
  /** 参与策略匹配的主体，通常是命令行或文件路径 */
  subjects: string[];
  /** 给人看的预览：命令原文、补丁摘要 */
  preview: string;
}

export type ApprovalDecision = "allow_once" | "allow_always" | "deny";

export type ApprovalHandler = (req: ApprovalRequest) => Promise<ApprovalDecision>;

/** 没有 handler 时的默认行为：拒绝，并说清楚原因。 */
export const denyingHandler: ApprovalHandler = async () => "deny";

export function denialMessage(tool: string, preview: string): string {
  return [
    `The user did not approve this ${tool} call:`,
    preview.length > 400 ? `${preview.slice(0, 400)}…` : preview,
    "",
    "Do not retry the same action. Either take a different approach, or stop and",
    "explain to the user what you need and why.",
  ].join("\n");
}
