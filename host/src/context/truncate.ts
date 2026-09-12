// Middle truncation: keep the head and the tail, cut the middle.
//
// The most informative parts of command output are the beginning (what ran,
// the first few errors) and the end (the final state, the test verdict, the
// stack trace). Keeping only the head loses the conclusion; keeping only the
// tail loses the context.
export function truncateMiddle(text: string, headChars: number, tailChars: number): string {
  if (text.length <= headChars + tailChars) return text;
  const head = text.slice(0, headChars);
  const tail = text.slice(-tailChars);
  const dropped = text.length - headChars - tailChars;
  return `${head}\n\n… [${dropped} characters omitted] …\n\n${tail}`;
}
