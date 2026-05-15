(* 31V4 Assignment 2 - PDP subset assembler *)
(* Written by: J.G.Harston, (C) HCE         *)

(* Parser module. Entry at Do_This_Line to parse and decode the current *)
(* line and call the appropriate routine for what it is. *)

IMPLEMENTATION MODULE Parser;
FROM FileSystem IMPORT Reset;
FROM InOut IMPORT in,Read,Done,OpenInput,CloseInput,
         WriteString, WriteOct, WriteLn, WriteInt, ReadInt;
FROM Assem IMPORT do_op_code, match;
FROM Symbols IMPORT enter_label, Label;

VAR EOF,fatal,endflg:BOOLEAN;
    last_ch:CHAR;

PROCEDURE get_line; (* Reads a line into txt_buf *)
VAR ptr:INTEGER;
    ch:CHAR;
BEGIN
  ptr:=0; ln_ptr:=0;
  REPEAT
    Read(ch);
    txt_buf[ptr]:=ch;
    ptr:=ptr+1;
  UNTIL (ch=15c) OR (ptr>79) OR (NOT Done);
       (* Keep going until a whole line, 80 characters or the end of the file *)
  IF NOT Done THEN EOF:=TRUE; END;
  last_ch:=ch;
  IF last_ch=15c THEN
    txt_buf[ptr-1]:=0c;  (* Terminate the string *)
  END;
END get_line;

PROCEDURE get_symbol(VAR txt_ln:txtln):Symbol_Type;
(* ln_ptr points to current char in line *)
VAR ptr:INTEGER;
    ch:CHAR;
    retval:Symbol_Type;
BEGIN
  skip_space; (* Move past any spaces *)
  ptr:=0;
  REPEAT
    ch:=txt_buf[ln_ptr];
    txt_ln[ptr]:=upper(ch);
    ptr:=ptr+1; ln_ptr:=ln_ptr+1;
  UNTIL (ch=' ') OR (ch=':') OR (ch='=') OR (ch=';')
        OR (ch=11c) OR (ch=',') OR (ptr>10) OR (ch=0c);
  IF ch=11c THEN txt_ln[ptr-1]:=' '; ch:=' '; END; (* TAB *)
  txt_ln[ptr]:=0;
  IF txt_ln[0]=';' THEN RETURN comment; END; (* ;comment *)
  IF txt_ln[0]=0c THEN RETURN comment; END;
(* ^^^ nul line *)
  IF txt_ln[0]='.' THEN RETURN pseudo; END; (* .pseudo *)
  retval:=error_txt;
(* Now check final chars: *)
  IF (ch=';') OR (ch=0c) THEN
    ln_ptr:=ln_ptr-1; (* Move back to point to the ';' for next time *)
    RETURN op_code;
  END;
  IF ch=':' THEN retval:=label; END;
  IF ch='=' THEN retval:=equate; END;
  IF ch=',' THEN
    retval=src_addr;
    ln_ptr:=ln_ptr-1; (* move pointer to point to the comma *)
    txt_ln[ptr-1]:=0c;
  END;
  IF ch=' ' THEN (* See if there's a space following *)
    skipspace;
    IF txt_buf[ln_ptr]=',' THEN
      retval:=src_addr;
    ELSE
      retval:=op_code;
    END;
  END;
  RETURN retval;
END get_symbol;

PROCEDURE skip_space; (* Increments ln_ptr past any spaces and tabs *)
BEGIN
  WHILE ((txt_buf[ln_ptr]=' ') OR (txt_buf[ln_ptr]=11c))
        AND (ln_ptr<=buf_len) DO
    ln_ptr:=ln_ptr+1;
  END;
  IF ln_ptr>buf_len THEN   (* If past the end of the buffer, point to *)
    txt_buf[buf_len]:=0c;  (* a termination character *)
    ln_ptr:=buf_len;
  END;
END skip_space;

PROCEDURE Disp_Line;
(* Displays the text buffer, and any characters not read in *)
VAR ch:CHAR
BEGIN
  WriteString(txt_buf);
  IF last_ch<>15c THEN
    REPEAT
      Read(ch);
      WriteString(ch);
    UNTIL (ch=15c) OR (NOT Done);
    IF NOT Done THEN EOF:=TRUE; END;
   ELSE
    WriteLn;
  END;
END Disp_Line;

PROCEDURE upper(ch:CHAR):CHAR; (* Forces upper case *)
BEGIN
  IF (ch>='a') AND (ch<='z') THEN RETURN CHR(ORD(ch)-32)
   ELSE RETURN ch; END;
END upper;

PROCEDURE Do_This_Line(Pass,line_num:INTEGER):BOOLEAN; (* Returns error flag *)
VAR symbol:SymbolType;
    equ_flag:BOOLEAN;
    loop:INTEGER;
BEGIN
  words:=0; equ_flg:=FALSE; error:=FALSE; fatal:=FALSE;
  EOF:=FALSE; no_label:=FALSE;
  Assign(mesg_txt,'.'); (* Initially set up as just a full stop *)
  get_line; (* read in a line *)
  ln_ptr:=0;
  symbol:=get_symbol(text); (* Get the first symbol *)
  IF EOF THEN RETURN FALSE; END; (* exit at end of file *)
  IF symbol=pseudo THEN do_pseudo; symbol:=comment; END;
  IF symbol=equate THEN
    IF NOT endflg THEN (* For equate, check if we are in the program *)
      error:=TRUE;
      Assign(err_txt,'Equates only allowed at start of program');
     ELSE
      do_equate; equ_flg:=TRUE;
      symbol:=comment;
    END;
  END;
  IF symbol=label THEN
    do_label;      (* Process the label *)
    IF error THEN
      symbol:=comment; (* We had a illegal label declaration. *)
     ELSE
      symbol:=get_symbol(text); (* Get next symbol *)
    END;
  END;
  IF symbol=op_code THEN
    IF PC<0 THEN   (* If PC<0, then we havn't had a .start yet *)
      fatal:=TRUE; (* This is fatal. We don't know where the code is going *)
      error:=TRUE;
      Assign(err_txt,'*** No .start directive');
     ELSE
      do_op_code;  (* Do the opcode, and then get the next symbol *)
      symbol:=get_symbol(text);
    END;
  END;
  IF Pass=2 THEN   (* Only on pass two do any printing *)
    WriteInt(line_num,2); (* The line number *)
    WriteString(' ');
    IF words<>0 THEN      (* If an opcode generated some code, display the *)
      WriteOct(PC,5);     (* PC value *)
     ELSE
      WriteString('     ');
    END;
    WriteString(' ');
    IF (words<>0) OR (equ_flg) THEN
      WriteOct(cache[0],6);  (* Display the first code word, or the value of
                                an equate *)
     ELSE
      WriteString('      ');
    END;
    WriteString(' ');
    Disp_Line;     (* Display the line *)
    IF words>1 THEN (* If the opcode generated more than 1 word, display the
                       rest of the line *)
      FOR loop:=1 TO words-1 DO
        WriteString('         ');
        WriteOct(cache[loop],6);
        WriteLn;
       END;
    END;
    IF error OR no_label THEN (* If we had an error or an undefined label, *)
      WriteString(err_txt);   (* then display the message. *)
      WriteString(mesg_txt);  (* This may have been set to indicate source or
                                 dest field *)
      WriteLn;
    END;
    IF (symbol<>comment) AND (error=FALSE) THEN
      WriteString('Line terminated strangely.'); WriteLn;
                            (* If the last thing on the line isn't a comment,
                               then something's wrong somewhere *)
    END;
  END;
  PC:=PC+words*2; (* Increment the PC by twice the number of words. *)
  RETURN fatal;   (* Return the fatal flag *)
END Do_This_Line;

PROCEDURE do_pseudo;
VAR num,temp:INTEGER;
    symbol:Symbol_Type;
BEGIN
  num:=match(text,'.START,0,.END,1,+');
  CASE num OF
    0 : symbol:=get_symbol(text);
        temp:=0;
        PC:=FindOct(text,temp);
        PC:=2*(PC DIV 2); (* Ensure is on an even byte boundary *)
        endflg:=error;    (* If no error, then we're not at the end *)
        IF error THEN PC:=PC-1; END;
    1 : endflg:=TRUE;     (* We're at the end *)
        fatal:=TRUE;      (* Bodge to signal the end of the program *)
   ELSE
    error:=TRUE;
    Assign(err_txt,'Unknown pseudo-instruction');
  END;
END do_pseudo;

PROCEDURE do_equate;
VAR symbol:Symbol_Type;
    eq_num:txtln;
    value,temp:INTEGER;
BEGIN
  symbol:=get_symbol(eq_num);
  temp:=0;
  value:=FindOct(eq_num,temp); (* Read in the value *)
  SetSymbol(text,'=',value); (* Set the symbol to the value *)
  cache[0]:=value;
END do_equate;

PROCEDURE do_label;
BEGIN
  SetSymbol(text,':',PC);  (* Set the symbol to the PC value *)
END do_label;

PROCEDURE SetSymbol(text:txtln, endchr:CHAR, value:INTEGER);
VAR ptr:INTEGER;
    labl:Label;
BEGIN
  ptr:=0;
  REPEAT (* Copy the text into the label up to the termination char supplied *)
    labl[ptr]:=text[ptr];
    ptr:=ptr+1;
  UNTIL text[ptr]=endchr;
  labl[ptr]=0c;
  IF lab[0]<'A' THEN   (* Check the label starts with a letter *)
                       (* The above ignores [ \ ] ^ _ ' { | } ~ but that's ok *)
    error:=TRUE;
    Assign(err_txt,'Invalid label name');
   ELSE
    enter_label(labl,value); (* Enter it into the symbol table *)
  END;
END SetSymbol;

(* General line-decoding routines: *)

PROCEDURE FindOct(string:ARRAY OF CHAR; VAR ptr:INTEGER):INTEGER;
(* Get an octal value. Give an error if no ocatal there *)
(* ptr points to the first character of the number *)
BEGIN
  IF (string[ptr]<'0') OR (string[ptr]>'7') THEN
    error:=TRUE;
    Assign(err_txt,'Invalid octal number');
    RETURN 0;
   ELSE
    RETURN GetOct(string,ptr);
  END;
END FindOct;

PROCEDURE GetOct(string: ARRAY OF CHAR; VAR ptr:INTEGER):INTEGER;
(* Gets an octal value from the string, starting at ptr. Assumes that
   the char pointed to by ptr is an octal digit. Return ptr pointing
   to the first non-octal digit (or past HIGH). *)
VAR value,temp:INTEGER;
BEGIN
  value:=0:
  REPEAT
    temp:=ORD(string[ptr]) MOD 8;
    value:=value*8+temp;
    ptr:=ptr+1;
  UNTIL (string[ptr]<'0') OR (string[ptr]>'7') OR (ptr>HIGH(string));
  RETURN value;
END GetOct;

PROCEDURE Assign(VAR outtxt:ARRAY OF CHAR; intxt:ARRAY OF CHAR);
(* Do outtxt:=intxt by copying strings *)
VAR ptr:INTEGER;
    ch:CHAR;
BEGIN
  ptr:=0;
  REPEAT
    outtxt[ptr]:=intxt[ptr];
    ptr:=ptr+1;
  UNTIL (intxt[ptr-1]=0c) OR (ptr>HIGH(intxt)) OR (ptr>HIGH(outtxt));
  IF ptr<HIGH(outtxt) THEN
    outtxt[ptr]:=0c;
  END;
END Assign;

BEGIN
  PC:=1;      (* Initial state of no .start and beginning of program *)
  endflg:=TRUE;
END Parser.