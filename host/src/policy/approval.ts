// The approval loop: ask -> suspend -> question -> resume.
//
// Two design decisions:
//
// 1. In a non-interactive environment ask is equivalent to deny, not allow.
//    Allowing something when nobody can answer is the same as the rule never
//    having existed.
//
// 2. A denial is *fed back to the model*, not thrown.
//    The model needs to know "the user has closed off this route" in order to
//    think of another one; throwing just fails the whole turn and the user has
//    to start over.
export interface ApprovalRequest {
  id: string;
  tool: string;
  /** The subjects policy matching runs against, usually a command line or file
   *  path */
  subjects: string[];
  /** The preview shown to a human: the command verbatim, a patch summary */
  preview: string;
}

export type ApprovalDecision = "allow_once" | "allow_always" | "deny";

export type ApprovalHandler = (req: ApprovalRequest) => Promise<ApprovalDecision>;

/** The default behaviour with no handler: deny, and say why. */
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
