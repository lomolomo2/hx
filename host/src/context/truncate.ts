// 中段截断：保住头和尾，砍中间。
//
// 命令输出里最有信息量的是开头（跑了什么、前几条错误）和结尾（最终状态、
// 测试结论、堆栈）。只留头会丢掉结论，只留尾会丢掉上下文。
export function truncateMiddle(text: string, headChars: number, tailChars: number): string {
  if (text.length <= headChars + tailChars) return text;
  const head = text.slice(0, headChars);
  const tail = text.slice(-tailChars);
  const dropped = text.length - headChars - tailChars;
  return `${head}\n\n… [${dropped} characters omitted] …\n\n${tail}`;
}
