# imapshell

Operate on an IMAP mailbox: list, show, save or delete mails.
Mails can be selected by index, subject, sender, receiver, date, content.

Interactive shell by default.
Not interactive with ``-q``.


## Operation

- give search criteria (index, subject, sender, receiver, date, content)

- set operation flags and options

- run operation (list, show, save, delete)


## Non-interactive mode

Non interactive mode (option ``-q``) runs a single operation,
by default listing the selected emails.
Option ``-k`` changes it into either showing, saving or deleting.
Some examples:

- ``imapshell -f Sam -s news -q -- robert@gmail.com -100``  
show the the mail from ``Sam`` with subject containing ``news``
among the last 100 mails

- ``imapshell -a 12-Dec-2024 -k 'save body' -q -- jack@aol.com 1``  
save the body of all emails dated after December, 12 to files ``body.n``

- ``imapshell -t Luke -w -k delete -q -- laura@proton.me -200``  
delete all mails sent to Luke upon confirmation


## Interactive mode

The mails to list, show, save and delete are selected by
``from``, ``to``, ``cc``, ``subject``, ``after``, ``before``,
``on``, ``text``, ``begin``, ``end`` and ``range``.
The operations to perform on them are
``list``, ``show``, ``save``, and ``delete``.

An example interactive session follows, where the user input follows the prompt
``IMAP>``. First the mail are selected by subject (``subject this``), then by
sender (``from sam``) and then saved to files (``save textfile``).


```
imapshell -v . -- mark@gmail.com -200
account mark@gmail.com in imap.gmail.com/INBOX
found 4 mails within -200-0 (1-4): 4 94 112 351 
1: about this
    sam <gmail@gmail.com>
    12-Dec-2024
2: Re: about this
    robert <aol@aol.com>
    12-Dec-2024
3: nothing
    sam <proton@proton.me>
    3-Jan-2025
4: some other
    frank <gitlab@gitlab.freedesktop.org>
    4-Jan-2025
current parameters:
UID SEARCH 1:4
execute- listonly- structure- body- restore+ synchronous- viewer=imapenvelope verbose- ro
IMAP> subject this
current parameters:
UID SEARCH 1:4 SUBJECT "this"
execute- listonly- structure- body- restore+ synchronous- viewer=imapenvelope verbose- ro
IMAP> from sam
current parameters:
UID SEARCH 1:4 FROM sam SUBJECT "this"
execute- listonly- structure- body- restore+ synchronous- viewer=imapenvelope verbose- ro
IMAP> save textfile
found 1 mail within -200-0 (1-4): 4 
===== UID 4 =====
1: about this
    sam <gmail@gmail.com>
    12-Dec-2024
A008 OK
content -> textfile.4
*** emails saved
*** uncompress with munpack (unpack package)
current parameters:
UID SEARCH 1:4 FROM sam SUBJECT "this"
execute- listonly- structure- bodytextfile restore+ synchronous- viewer=imapenvelope verbose- ro
IMAP> quit 
logout
```
