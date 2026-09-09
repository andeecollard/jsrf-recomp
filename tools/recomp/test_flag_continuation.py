"""Executable regression for flags split across straight-line continuations."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from . import config
from .translator import FunctionTranslator

BASE=0x10000
# cmp eax,0 / mov ecx,eax / jbe zero; mov eax,1; ret; zero: xor eax,eax; ret
IMAGE=bytes.fromhex('83f80089c17606b801000000c331c0c3')


def translator(image=IMAGE, sizes=(3,2,11)):
    config._install([config.Section('.text',BASE,len(image),0,len(image),True)],
                    entry_point=BASE,kernel_thunk_addr=BASE,origin='flag-continuation-test')
    db={};offset=0
    for size in sizes:
        address=BASE+offset
        db[address]={'start':f'0x{address:08X}','end':address+size,
                     '_addr':address,'size':size}
        offset+=size
    return FunctionTranslator(image,db),db


class FlagContinuationTest(unittest.TestCase):
    def setUp(self):
        saved=dict(config.__dict__)
        self.addCleanup(config.__dict__.update,saved)

    def test_three_fragments_execute_empty_and_nonempty_cases(self):
        ft,db=translator()
        source=ft.translate_function(BASE,db[BASE])
        self.assertIn('Inlined flag continuation entries: 0x00010003, 0x00010005',source)
        self.assertIn('CMP_BE(_fa, _fb)',source)
        self.assertNotIn('UNRESOLVED FLAGS',source)
        code='''#include <stdint.h>
uint32_t eax,ecx,esp;
#define CMP_BE(a,b) ((uint32_t)(a)<=(uint32_t)(b))
'''+source+'''
int main(void) {
uint32_t values[]={0,1,2,0x7fffffff,0x80000000,0xffffffff};
for(unsigned i=0;i<6;++i) {
 eax=values[i]; esp=100;
 sub_00010000();
 if(eax!=(values[i]!=0) || ecx!=values[i] || esp!=104) return 1;
}
return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/'test.c';exe=Path(tmp)/'test';c.write_text(code)
            subprocess.run([shutil.which('cc'),str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
        # Independent entry points remain present and do not borrow flags
        # from a previously executed invocation.
        standalone=ft.translate_function(BASE+5,db[BASE+5])
        self.assertIn('void sub_00010005(void)',standalone)
        self.assertIn('UNRESOLVED FLAGS',standalone)
        self.assertEqual(db[BASE]['end'],BASE+3)

    def test_manual_target_is_not_inlined(self):
        ft,db=translator()
        ft.lifter.manual_functions.add(BASE+3)
        source=ft.translate_function(BASE,db[BASE])
        self.assertNotIn('Inlined flag continuation',source)

    def test_overwritten_flags_do_not_trigger_inlining(self):
        # Replace transparent MOV with a fresh CMP; the old flags are dead.
        ft,db=translator(bytes.fromhex('83f80083f8027606b801000000c331c0c3'),(3,3,11))
        self.assertNotIn('Inlined flag continuation',ft.translate_function(BASE,db[BASE]))

    def test_control_flow_before_boundary_is_not_assumed_straight(self):
        ft,db=translator(bytes.fromhex('eb0183f80089c17606b801000000c331c0c3'),(5,2,11))
        self.assertNotIn('Inlined flag continuation',ft.translate_function(BASE,db[BASE]))

    def test_cfg_converges_on_final_test_before_boundary(self):
        # Same relevant shape as JSRF 0x512F0 -> 0x51402: earlier conditional
        # paths join, then every path reaching the split executes a fresh TEST.
        image=bytes.fromhex(
            '83f800'              # cmp eax,0
            '7402'                # je final_test
            '89c1'                # mov ecx,eax
            '85c0'                # final_test: test eax,eax
            '7506b801000000c331c0c3')
        ft,db=translator(image,(9,11))

        source=ft.translate_function(BASE,db[BASE])
        self.assertIn('Inlined flag continuation entries: 0x00010009',source)
        self.assertIn('/* test eax, eax (32-bit) */',source)
        self.assertIn('TEST_NZ(_fa, _fb)',source)
        self.assertNotIn('UNRESOLVED FLAGS',source)

        standalone=ft.translate_function(BASE+9,db[BASE+9])
        self.assertIn('UNRESOLVED FLAGS',standalone)

    def test_call_before_consumer_is_not_inlined(self):
        ft,db=translator(bytes.fromhex('83f800e8000000007606b801000000c331c0c3'),(3,5,11))
        self.assertNotIn('Inlined flag continuation',ft.translate_function(BASE,db[BASE]))

    def test_calls_before_final_compare_allow_inlining(self):
        # Same shape as JSRF 0x50860 -> 0x5089B: returning calls precede
        # xor/cmp, two MOVs preserve that CMP, and the next entry starts at JE.
        prefix=bytes.fromhex(
            'e82b000000'          # call BASE+0x30
            'e826000000'          # call BASE+0x30
            '31ff'                # xor edi,edi
            '39fd'                # cmp ebp,edi
            '89442410'            # mov [esp+0x10],eax
            '89bb98000000')       # mov [ebx+0x98],edi
        continuation=bytes.fromhex('7406b801000000c331c0c3')
        image=prefix+continuation+b'\x90'*(0x30-len(prefix)-len(continuation))+b'\xc3'
        config._install([config.Section('.text',BASE,len(image),0,len(image),True)],
                        entry_point=BASE,kernel_thunk_addr=BASE,
                        origin='flag-continuation-call-prefix-test')
        db={
            BASE:{'start':f'0x{BASE:08X}','end':BASE+len(prefix),
                  '_addr':BASE,'size':len(prefix)},
            BASE+len(prefix):{
                'start':f'0x{BASE+len(prefix):08X}',
                'end':BASE+len(prefix)+len(continuation),
                '_addr':BASE+len(prefix),'size':len(continuation)},
            BASE+0x30:{'start':f'0x{BASE+0x30:08X}','end':BASE+0x31,
                       '_addr':BASE+0x30,'size':1},
        }
        ft=FunctionTranslator(image,db)

        source=ft.translate_function(BASE,db[BASE])
        self.assertIn('Inlined flag continuation entries: 0x00010018',source)
        self.assertEqual(source.count('RECOMP_ABI_CALL'),2)
        self.assertIn('/* cmp ebp, edi (32-bit) */',source)
        self.assertIn('CMP_EQ(_fa, _fb)',source)
        self.assertNotIn('UNRESOLVED FLAGS',source)
        self.assertLess(source.index('RECOMP_ABI_CALL'),
                        source.index('/* cmp ebp, edi (32-bit) */'))

        # The independent entry still has no predecessor snapshot to borrow.
        standalone=ft.translate_function(BASE+len(prefix),
                                         db[BASE+len(prefix)])
        self.assertIn('UNRESOLVED FLAGS',standalone)

    def test_consumed_fragment_can_publish_fresh_test_to_next_entry(self):
        # Same shape as JSRF 0x5089B -> 0x508DF: the first split begins with a
        # conditional exit, its fallthrough calls a function, then a fresh TEST
        # immediately precedes a second split whose first instruction is JE.
        first=bytes.fromhex('83f800')
        middle=bytes.fromhex(
            '743b'                # je BASE+0x40 (exit from this fragment)
            'e838000000'          # call BASE+0x42
            '85c0')               # test eax,eax
        final=bytes.fromhex('7406b801000000c331c0c3')
        image=(first+middle+final
               + b'\x90'*(0x40-len(first)-len(middle)-len(final))
               + b'\xc3\x90\xc3')
        config._install([config.Section('.text',BASE,len(image),0,len(image),True)],
                        entry_point=BASE,kernel_thunk_addr=BASE,
                        origin='flag-continuation-chained-test')
        middle_start=BASE+len(first)
        final_start=middle_start+len(middle)
        db={
            BASE:{'start':f'0x{BASE:08X}','end':middle_start,
                  '_addr':BASE,'size':len(first)},
            middle_start:{'start':f'0x{middle_start:08X}',
                          'end':final_start,'_addr':middle_start,
                          'size':len(middle)},
            final_start:{'start':f'0x{final_start:08X}',
                         'end':final_start+len(final),'_addr':final_start,
                         'size':len(final)},
            BASE+0x40:{'start':f'0x{BASE+0x40:08X}','end':BASE+0x41,
                       '_addr':BASE+0x40,'size':1},
            BASE+0x42:{'start':f'0x{BASE+0x42:08X}','end':BASE+0x43,
                       '_addr':BASE+0x42,'size':1},
        }
        ft=FunctionTranslator(image,db)

        source=ft.translate_function(BASE,db[BASE])
        self.assertIn(
            'Inlined flag continuation entries: 0x00010003, 0x0001000C',
            source)
        self.assertIn('CMP_EQ(_fa, _fb)',source)
        self.assertIn('TEST_Z(_fa, _fb)',source)
        self.assertNotIn('UNRESOLVED FLAGS',source)
        self.assertLess(source.index('call 0x00010042'),
                        source.index('/* test eax, eax (32-bit) */'))
        self.assertLess(source.index('/* test eax, eax (32-bit) */'),
                        source.index('loc_0001000C:'))

        # As with the guest split, direct entry at the second fragment has no
        # incoming flag contract and must remain unresolved.
        standalone=ft.translate_function(final_start,db[final_start])
        self.assertIn('UNRESOLVED FLAGS',standalone)

    def test_fresh_producer_at_next_boundary_keeps_prior_continuation(self):
        # Same shape as JSRF 0x523FF -> 0x52402: a split JE consumes CMP,
        # its fallthrough preserves flags, and the next split starts with a
        # fresh TEST. The proven JE continuation must not be rolled back merely
        # because carrying the old CMP into the TEST fragment is unnecessary.
        first=bytes.fromhex('3b6904')       # cmp ebp,[ecx+4]
        middle=bytes.fromhex(
            '7409'                         # je final target
            '8b4008')                      # mov eax,[eax+8]
        following=bytes.fromhex(
            '85c0'                         # test eax,eax
            '7502'                         # jne final target
            '90c3')                        # nop; ret
        image=first+middle+following+b'\xc3'
        ft,db=translator(image,(len(first),len(middle),len(following),1))
        middle_start=BASE+len(first)
        following_start=middle_start+len(middle)

        source=ft.translate_function(BASE,db[BASE])
        self.assertIn(
            f'Inlined flag continuation entries: 0x{middle_start:08X}',source)
        self.assertNotIn(f'0x{middle_start:08X}, 0x{following_start:08X}',source)
        self.assertIn('/* cmp ebp, MEM32(ecx + 4) (32-bit) */',source)
        self.assertIn('CMP_EQ(_fa, _fb)',source)
        self.assertIn(f'sub_{following_start:08X}(); return;',source)
        self.assertNotIn('UNRESOLVED FLAGS',source)

        # Direct entry at the consumed JE still has no incoming flag contract.
        standalone=ft.translate_function(middle_start,db[middle_start])
        self.assertIn('UNRESOLVED FLAGS',standalone)

    def test_incompatible_tail_jump_entry_flags_are_not_borrowed(self):
        # Same relevant shape as JSRF 0x749DA/0x74AEC -> 0x749DC. One owner
        # falls through from TEST eax,eax; another reaches the shared JE via a
        # flag-preserving JMP after CMP eax,edi. Their ZF meanings differ.
        image=(bytes.fromhex('3bc7e90b000000')  # cmp eax,edi; jmp shared JE
               + b'\x90'*9
               + bytes.fromhex(
                   '85c0'                    # test eax,eax
                   '7401'                    # shared JE
                   'c3c3'))
        config._install([config.Section('.text',BASE,len(image),0,len(image),True)],
                        entry_point=BASE,kernel_thunk_addr=BASE,
                        origin='flag-continuation-incompatible-tail-entry')
        shared=BASE+0x12
        fallthrough_owner=BASE+0x10
        db={
            BASE:{'start':f'0x{BASE:08X}','end':BASE+7,
                  '_addr':BASE,'size':7},
            fallthrough_owner:{'start':f'0x{fallthrough_owner:08X}',
                               'end':BASE+len(image),
                               '_addr':fallthrough_owner,'size':6},
            shared:{'start':f'0x{shared:08X}','end':BASE+len(image),
                    '_addr':shared,'size':4},
        }
        ft=FunctionTranslator(image,db)

        fallthrough=ft.translate_function(fallthrough_owner,
                                           db[fallthrough_owner])
        self.assertIn('TEST_Z(_fa, _fb)',fallthrough)
        self.assertNotIn('UNRESOLVED FLAGS',fallthrough)

        tail_owner=ft.translate_function(BASE,db[BASE])
        self.assertIn(f'sub_{shared:08X}(); return;',tail_owner)
        standalone=ft.translate_function(shared,db[shared])
        self.assertIn('UNRESOLVED FLAGS',standalone)

        # A concrete witness that no single borrowed predicate is valid.
        eax=edi=5
        self.assertNotEqual(eax == 0,eax == edi)

    def test_owned_entry_is_not_inlined(self):
        ft,db=translator()
        ft.owned_function_starts.add(BASE+3)
        self.assertNotIn('Inlined flag continuation',ft.translate_function(BASE,db[BASE]))


if __name__=='__main__':
    unittest.main()
