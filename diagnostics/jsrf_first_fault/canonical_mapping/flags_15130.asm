00015130 83ec08               sub esp, 8
00015133 53                   push ebx
00015134 8b590c               mov ebx, dword ptr [ecx + 0xc]
00015137 55                   push ebp
00015138 33ed                 xor ebp, ebp
0001513A 3bdd                 cmp ebx, ebp
0001513C 894c2408             mov dword ptr [esp + 8], ecx
00015140 0f849d020000         je 0x153e3
00015146 56                   push esi
00015147 57                   push edi
00015148 eb06                 jmp 0x15150
0001514A 8d9b00000000         lea ebx, [ebx]
00015150 8b4104               mov eax, dword ptr [ecx + 4]
00015153 3bc5                 cmp eax, ebp
00015155 89442414             mov dword ptr [esp + 0x14], eax
00015159 0f8477020000         je 0x153d6
0001515F 90                   nop 
00015160 8b03                 mov eax, dword ptr [ebx]
00015162 8b742414             mov esi, dword ptr [esp + 0x14]
00015166 396c8604             cmp dword ptr [esi + eax*4 + 4], ebp
0001516A 753c                 jne 0x151a8
0001516C 6a14                 push 0x14
0001516E e87d570300           call 0x4a8f0
00015173 8b0b                 mov ecx, dword ptr [ebx]
00015175 89448e04             mov dword ptr [esi + ecx*4 + 4], eax
00015179 8b13                 mov edx, dword ptr [ebx]
0001517B 8b449604             mov eax, dword ptr [esi + edx*4 + 4]
0001517F 8928                 mov dword ptr [eax], ebp
00015181 8b0b                 mov ecx, dword ptr [ebx]
00015183 8b548e04             mov edx, dword ptr [esi + ecx*4 + 4]
00015187 896a04               mov dword ptr [edx + 4], ebp
0001518A 8b03                 mov eax, dword ptr [ebx]
0001518C 8b4c8604             mov ecx, dword ptr [esi + eax*4 + 4]
00015190 896908               mov dword ptr [ecx + 8], ebp
00015193 8b13                 mov edx, dword ptr [ebx]
00015195 8b449604             mov eax, dword ptr [esi + edx*4 + 4]
00015199 89680c               mov dword ptr [eax + 0xc], ebp
0001519C 8b0b                 mov ecx, dword ptr [ebx]
0001519E 8b548e04             mov edx, dword ptr [esi + ecx*4 + 4]
000151A2 83c404               add esp, 4
000151A5 896a10               mov dword ptr [edx + 0x10], ebp
000151A8 8b03                 mov eax, dword ptr [ebx]
000151AA 8b748604             mov esi, dword ptr [esi + eax*4 + 4]
000151AE 3bf5                 cmp esi, ebp
000151B0 0f8406020000         je 0x153bc
000151B6 396e04               cmp dword ptr [esi + 4], ebp
000151B9 0f85fd010000         jne 0x153bc
000151BF 392e                 cmp dword ptr [esi], ebp
000151C1 0f85f5010000         jne 0x153bc
000151C7 8b4c2414             mov ecx, dword ptr [esp + 0x14]
000151CB 8b7e08               mov edi, dword ptr [esi + 8]
000151CE 8b11                 mov edx, dword ptr [ecx]
000151D0 8b02                 mov eax, dword ptr [edx]
000151D2 c1e705               shl edi, 5
000151D5 03f8                 add edi, eax
000151D7 8b07                 mov eax, dword ptr [edi]
000151D9 83f809               cmp eax, 9
000151DC 0f87da010000         ja 0x153bc
000151E2 ff2485ec530100       jmp dword ptr [eax*4 + 0x153ec]
000151E9 8b4f14               mov ecx, dword ptr [edi + 0x14]
000151EC 8b5710               mov edx, dword ptr [edi + 0x10]
000151EF 51                   push ecx
000151F0 8b4c2414             mov ecx, dword ptr [esp + 0x14]
000151F4 8d4304               lea eax, [ebx + 4]
000151F7 52                   push edx
000151F8 50                   push eax
000151F9 e842feffff           call 0x15040
000151FE 85c0                 test eax, eax
00015200 7420                 je 0x15222
00015202 837b1001             cmp dword ptr [ebx + 0x10], 1
00015206 0f85b0010000         jne 0x153bc
0001520C d94314               fld dword ptr [ebx + 0x14]
0001520F d84610               fadd dword ptr [esi + 0x10]
00015212 d95610               fst dword ptr [esi + 0x10]
00015215 d85f04               fcomp dword ptr [edi + 4]
00015218 dfe0                 fnstsw ax
0001521A f6c401               test ah, 1
0001521D e987010000           jmp 0x153a9
00015222 8b471c               mov eax, dword ptr [edi + 0x1c]
00015225 8b4f18               mov ecx, dword ptr [edi + 0x18]
00015228 50                   push eax
00015229 51                   push ecx
0001522A 8b4c2418             mov ecx, dword ptr [esp + 0x18]
0001522E 8d4304               lea eax, [ebx + 4]
00015231 50                   push eax
00015232 e809feffff           call 0x15040
00015237 85c0                 test eax, eax
00015239 0f847d010000         je 0x153bc
0001523F c7460401000000       mov dword ptr [esi + 4], 1
00015246 896e08               mov dword ptr [esi + 8], ebp
00015249 896e10               mov dword ptr [esi + 0x10], ebp
0001524C 896e0c               mov dword ptr [esi + 0xc], ebp
0001524F e968010000           jmp 0x153bc
00015254 8b5714               mov edx, dword ptr [edi + 0x14]
00015257 8b4f10               mov ecx, dword ptr [edi + 0x10]
0001525A 52                   push edx
0001525B 51                   push ecx
0001525C 8b4c2418             mov ecx, dword ptr [esp + 0x18]
00015260 8d4304               lea eax, [ebx + 4]
00015263 50                   push eax
00015264 e8d7fdffff           call 0x15040
00015269 85c0                 test eax, eax
0001526B 0f840a010000         je 0x1537b
00015271 837b1001             cmp dword ptr [ebx + 0x10], 1
00015275 0f8541010000         jne 0x153bc
0001527B d9460c               fld dword ptr [esi + 0xc]
0001527E d80548451c00         fadd dword ptr [0x1c4548]
00015284 d9560c               fst dword ptr [esi + 0xc]
00015287 d85f08               fcomp dword ptr [edi + 8]
0001528A dfe0                 fnstsw ax
0001528C f6c401               test ah, 1
0001528F e915010000           jmp 0x153a9
00015294 8b4f14               mov ecx, dword ptr [edi + 0x14]
00015297 8b5710               mov edx, dword ptr [edi + 0x10]
0001529A 51                   push ecx
0001529B 8b4c2414             mov ecx, dword ptr [esp + 0x14]
0001529F 8d4304               lea eax, [ebx + 4]
000152A2 52                   push edx
000152A3 50                   push eax
000152A4 e897fdffff           call 0x15040
000152A9 85c0                 test eax, eax
000152AB 0f8471ffffff         je 0x15222
000152B1 837b1002             cmp dword ptr [ebx + 0x10], 2
000152B5 0f8501010000         jne 0x153bc
000152BB d94314               fld dword ptr [ebx + 0x14]
000152BE d84610               fadd dword ptr [esi + 0x10]
000152C1 d95610               fst dword ptr [esi + 0x10]
000152C4 d85f04               fcomp dword ptr [edi + 4]
000152C7 dfe0                 fnstsw ax
000152C9 f6c401               test ah, 1
000152CC e9d8000000           jmp 0x153a9
000152D1 8b5714               mov edx, dword ptr [edi + 0x14]
000152D4 8b4f10               mov ecx, dword ptr [edi + 0x10]
000152D7 52                   push edx
000152D8 51                   push ecx
000152D9 8b4c2418             mov ecx, dword ptr [esp + 0x18]
000152DD 8d4304               lea eax, [ebx + 4]
000152E0 50                   push eax
000152E1 e85afdffff           call 0x15040
000152E6 85c0                 test eax, eax
000152E8 0f848d000000         je 0x1537b
000152EE 837b1002             cmp dword ptr [ebx + 0x10], 2
000152F2 eb81                 jmp 0x15275
000152F4 8b4f14               mov ecx, dword ptr [edi + 0x14]
000152F7 8b5710               mov edx, dword ptr [edi + 0x10]
000152FA 51                   push ecx
000152FB 8b4c2414             mov ecx, dword ptr [esp + 0x14]
000152FF 8d4304               lea eax, [ebx + 4]
00015302 52                   push edx
00015303 50                   push eax
00015304 e837fdffff           call 0x15040
00015309 85c0                 test eax, eax
0001530B 0f8411ffffff         je 0x15222
00015311 837b1003             cmp dword ptr [ebx + 0x10], 3
00015315 e98f000000           jmp 0x153a9
0001531A 8b5714               mov edx, dword ptr [edi + 0x14]
0001531D 8b4f10               mov ecx, dword ptr [edi + 0x10]
00015320 52                   push edx
00015321 51                   push ecx
00015322 8b4c2418             mov ecx, dword ptr [esp + 0x18]
00015326 8d4304               lea eax, [ebx + 4]
00015329 50                   push eax
0001532A e811fdffff           call 0x15040
0001532F 85c0                 test eax, eax
00015331 7448                 je 0x1537b
00015333 837b1004             cmp dword ptr [ebx + 0x10], 4
00015337 eb70                 jmp 0x153a9
00015339 8b4f14               mov ecx, dword ptr [edi + 0x14]
0001533C 8b5710               mov edx, dword ptr [edi + 0x10]
0001533F 51                   push ecx
00015340 8b4c2414             mov ecx, dword ptr [esp + 0x14]
00015344 8d4304               lea eax, [ebx + 4]
00015347 52                   push edx
00015348 50                   push eax
00015349 e8f2fcffff           call 0x15040
0001534E 85c0                 test eax, eax
00015350 0f84ccfeffff         je 0x15222
00015356 837b1005             cmp dword ptr [ebx + 0x10], 5
0001535A eb4d                 jmp 0x153a9
0001535C 8b5714               mov edx, dword ptr [edi + 0x14]
0001535F 8b4f10               mov ecx, dword ptr [edi + 0x10]
00015362 52                   push edx
00015363 51                   push ecx
00015364 8b4c2418             mov ecx, dword ptr [esp + 0x18]
00015368 8d4304               lea eax, [ebx + 4]
0001536B 50                   push eax
0001536C e8cffcffff           call 0x15040
00015371 85c0                 test eax, eax
00015373 7406                 je 0x1537b
00015375 837b1006             cmp dword ptr [ebx + 0x10], 6
00015379 eb2e                 jmp 0x153a9
0001537B 8b571c               mov edx, dword ptr [edi + 0x1c]
0001537E 8b4718               mov eax, dword ptr [edi + 0x18]
00015381 52                   push edx
00015382 50                   push eax
00015383 e9a2feffff           jmp 0x1522a
00015388 8b4f14               mov ecx, dword ptr [edi + 0x14]
0001538B 8b5710               mov edx, dword ptr [edi + 0x10]
0001538E 51                   push ecx
0001538F 8b4c2414             mov ecx, dword ptr [esp + 0x14]
00015393 8d4304               lea eax, [ebx + 4]
00015396 52                   push edx
00015397 50                   push eax
00015398 e8a3fcffff           call 0x15040
0001539D 85c0                 test eax, eax
0001539F 0f847dfeffff         je 0x15222
000153A5 837b1007             cmp dword ptr [ebx + 0x10], 7
000153A9 7511                 jne 0x153bc
000153AB ff4608               inc dword ptr [esi + 8]
000153AE 896e10               mov dword ptr [esi + 0x10], ebp
000153B1 896e0c               mov dword ptr [esi + 0xc], ebp
000153B4 eb06                 jmp 0x153bc
000153B6 c70601000000         mov dword ptr [esi], 1
000153BC 8b542414             mov edx, dword ptr [esp + 0x14]
000153C0 8b8288000000         mov eax, dword ptr [edx + 0x88]
000153C6 3bc5                 cmp eax, ebp
000153C8 89442414             mov dword ptr [esp + 0x14], eax
000153CC 0f858efdffff         jne 0x15160
000153D2 8b4c2410             mov ecx, dword ptr [esp + 0x10]
000153D6 8b5b1c               mov ebx, dword ptr [ebx + 0x1c]
000153D9 3bdd                 cmp ebx, ebp
000153DB 0f856ffdffff         jne 0x15150
000153E1 5f                   pop edi
000153E2 5e                   pop esi
000153E3 5d                   pop ebp
000153E4 5b                   pop ebx
000153E5 83c408               add esp, 8
000153E8 c3                   ret 
000153E9 8d4900               lea ecx, [ecx]
000153EC e951010054           jmp 0x54015542
000153F1 52                   push edx
000153F2 0100                 add dword ptr [eax], eax
000153F4 94                   xchg esp, eax
000153F5 52                   push edx
000153F6 0100                 add dword ptr [eax], eax
000153F8 d15201               rcl dword ptr [edx + 1]
000153FB 00f4                 add ah, dh
000153FD 52                   push edx
000153FE 0100                 add dword ptr [eax], eax
00015400 1a5301               sbb dl, byte ptr [ebx + 1]
00015403 0039                 add byte ptr [ecx], bh
00015405 53                   push ebx
00015406 0100                 add dword ptr [eax], eax
00015408 5c                   pop esp
00015409 53                   push ebx
0001540A 0100                 add dword ptr [eax], eax
0001540C 885301               mov byte ptr [ebx + 1], dl
