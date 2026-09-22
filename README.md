# stupid-elf-packer

Um *packer* de ELF propositalmente simples, feito como prova de conceito. O objetivo não é ser robusto nem esperto. O objetivo é ser didático: mostrar, com o mínimo de código possível, o que acontece quando um executável é "empacotado" e depois desempacotado e executado em tempo de execução.

## O que é um packer?

Um packer pega um executável original, guarda ele (comprimido e/ou codificado) dentro de um novo executável e coloca junto um pequeno programa chamado **stub**. Quando você roda o binário empacotado:

1. O sistema operacional carrega e executa o **stub**, não o programa original.
2. O stub recupera o programa original que estava guardado (aqui, de forma bem ingênua, daí o "stupid").
3. O stub devolve o controle para o **entry point** original, e o programa roda como se nada tivesse acontecido.

Packers têm usos legítimos (redução de tamanho, proteção de propriedade intelectual) e são um exemplo excelente para ensinar **formato ELF**, **loader** e **layout de memória de um processo**.

## Estrutura do repositório

```
target/   Programa "alvo": um executável bobo que só imprime um texto.
stub/     O stub que roda primeiro e devolve o controle ao alvo original.
tool/     A ferramenta que junta alvo + stub e gera o binário empacotado.
shared/   Código/definições compartilhadas entre stub e tool.
```

## O alvo (target)

O alvo é o programa mais bobo possível, só para ter algo para empacotar:

```c
#include <stdio.h>

int main(void) {
    printf("hello, i am the packer target");
    return 42; // because that's the answer to everything, right?
}
```

Compilação:

```sh
gcc -static -no-pie -o target/target target/target.c
```

## Por que `-no-pie`?

Essa é a decisão de projeto mais importante do exemplo, então vale explicar com calma.

### Com `-no-pie` (o que usamos)

Com `-no-pie`, o binário é carregado sempre no mesmo endereço virtual fixo. Isso significa que o **entry point original é uma constante**: dá para saber exatamente onde ele está já na hora de empacotar.

Consequência: o stub devolve o controle ao programa original com **um único `jmp` para um endereço fixo**. Nada de cálculo, nada de descoberta em runtime. O stub fica o mais simples possível, que é justamente a intenção da POC.

### Com PIE (o que quebraria)

Um executável PIE (*Position Independent Executable*) é reposicionável: por causa do **ASLR**, o loader coloca a imagem numa base diferente a cada execução.

Um `jmp` com endereço "chumbado" no código quebra nesse cenário: na próxima execução o entry point original não está mais naquele endereço.

Para suportar PIE, o stub precisaria, em tempo de execução:

1. descobrir onde ele próprio foi carregado,
2. calcular a base real da imagem, e
3. ajustar (relocar) todos os endereços relativos a essa base.

Esse trabalho de relocação em runtime está fora do escopo deste exemplo. Ele é um ótimo "próximo passo" para quem quiser evoluir a POC, mas adicioná-lo agora só esconderia a ideia central atrás de complexidade desnecessária.

> **Resumo:** `-no-pie` significa endereço fixo, e o stub faz um `jmp` simples.
> PIE significa endereço mudando a cada execução, o que exigiria relocação em runtime.

## Por que `-static`?

Além do endereço fixo, também queremos um fluxo de inicialização previsível, e é por isso que compilamos com `-static`.

Num binário dinâmico, o kernel não entrega o controle direto ao programa. O ELF carrega um campo `PT_INTERP` apontando para o *dynamic linker* (por exemplo `/lib64/ld-linux-x86-64.so.2`), e é ele quem roda primeiro: mapeia as bibliotecas compartilhadas, resolve símbolos e só depois passa o controle ao programa. Isso é uma camada a mais entre "o kernel carregou o binário" e "o meu código roda".

Com `-static`, não há `PT_INTERP` nem loader dinâmico envolvido. O kernel carrega a imagem e entrega o controle **direto ao `_start`**. O fluxo é previsível e inteiramente contido no próprio binário, que é exatamente o que queremos para raciocinar sobre o packer sem a dor de cabeça de lidar com o loader dinâmico.

> **Resumo:** `-static` significa sem `PT_INTERP` e sem dynamic linker.
> O controle vai direto para o `_start`, num fluxo previsível.

## Inspecionando o alvo

Antes de empacotar qualquer coisa, precisamos saber ler a estrutura do binário. As ferramentas abaixo (`file` e `readelf`) mostram tudo o que o packer vai precisar.

> **Por que o binário `target/target` está versionado no repositório?**
> Normalmente não se commita artefato de build, mas aqui é proposital. Os valores mostrados nas saídas abaixo (entry point, offsets e tamanhos dos segmentos, BuildID) são fixos para *este* binário, mas dependem do toolchain que o gerou: recompilar com outra versão de gcc/ld/glibc, com outras flags ou com outro linker pode deslocar o layout e mudar esses números. Versionar o binário garante que, ao rodar os comandos deste README, você veja exatamente os mesmos dados aqui documentados. (Os comandos `file`/`readelf` só *leem* o binário; quem muda os valores é a recompilação, não a execução deles.)

### `file target`

```
ELF 64-bit LSB executable, x86-64, version 1 (GNU/Linux), statically linked, BuildID[sha1]=d4980fe039ab762df9db4ce13f31f19c08a5217a, for GNU/Linux 4.4.0, not stripped
```

Confirma o que compilamos: é um executável ELF de 64 bits e está **`statically linked`** (efeito do `-static`). Ou seja, sem dependência de loader dinâmico, como discutido acima.

### `readelf -h target` (cabeçalho ELF)

```
ELF Header:
  ...
  Type:                              EXEC (Executable file)
  Machine:                           Advanced Micro Devices X86-64
  Entry point address:               0x402e40
  Start of program headers:          64 (bytes into file)
  Number of program headers:         12
  ...
```

Dois campos interessam ao packer:

- **`Type: EXEC`** confirma que é um executável de endereço fixo (efeito do `-no-pie`). Se fosse PIE, apareceria `DYN`.
- **`Entry point address: 0x402e40`** é exatamente o endereço fixo para onde o stub vai dar o `jmp` no final. Como é `-no-pie`, esse valor é constante e conhecido na hora de empacotar.

### `readelf -l target` (program headers / segmentos)

Os *program headers* descrevem os segmentos que o kernel mapeia na memória. Os que importam:

```
  Type           VirtAddr           FileSiz            MemSiz             Flags
  LOAD           0x0000000000400000 0x0000000000000518 0x0000000000000518 R
  LOAD           0x0000000000401000 0x000000000007e7fd 0x000000000007e7fd R E
  LOAD           0x0000000000480000 0x00000000000342f8 0x00000000000342f8 R
  LOAD           0x00000000004b5168 0x00000000000058b0 0x000000000000b180 RW
  NOTE           0x00000000004002e0 0x0000000000000024 0x0000000000000024 R
  ...
```

Dois pontos centrais do plano do packer:

- **O segmento `LOAD` com flags `R E`** (`0x401000`) é o **código** (as seções `.init .plt .text .fini`). É esse segmento que será **cifrado** pelo packer. No binário empacotado ele fica ilegível até o stub decifrá-lo em runtime.
- **Um segmento `NOTE`** (por exemplo o `.note.gnu.build-id`) é apenas metadado descartável para a execução. O plano é **canibalizar esse `NOTE`**, reescrevendo a entrada do program header para transformá-lo num `PT_LOAD` que carrega o **stub**. Assim adicionamos o stub sem precisar realocar a tabela de program headers inteira.

### Executando o alvo

```sh
./target/target ; echo $?
```

```
hello, i am the packer target42
```

Sai o texto definido no `printf` e, logo em seguida, `42` (o `echo $?` mostra o código de saída, que é o `return 42` do `main`). Esse é o comportamento de referência: depois de empacotar e desempacotar, o binário tem que produzir **exatamente** essa mesma saída e esse mesmo código de retorno.

## A ferramenta (tool)

A `tool` é o programa que empacota o alvo: ela lê o ELF, cifra o segmento de código e injeta o stub. Ela é construída em passos, e cada passo é documentado aqui conforme entra.

### Estruturas do ELF que interessam

Um ELF de 64 bits, no que importa para este passo, tem três estruturas, todas definidas em `elf.h`:

- **`Elf64_Ehdr`** (ELF header): fica no offset `0` do arquivo. Ele dá o entry point original (OEP) e diz onde a tabela de program headers começa (`e_phoff`), quantas entradas ela tem (`e_phnum`) e o tamanho de cada entrada (`e_phentsize`).
- **`Elf64_Phdr`** (program header): cada um descreve um segmento. Os campos que usamos: `p_type` (é `PT_LOAD`? `PT_NOTE`?), `p_flags` (tem o bit de execução?), `p_offset` (onde no arquivo), `p_vaddr` (onde na memória), `p_filesz` e `p_memsz` (tamanhos).
- **A tabela de program headers** é só um array de `Elf64_Phdr` em sequência, começando no offset `e_phoff`.

### Passo 1: validar que a entrada é um ELF

Antes de interpretar qualquer campo, a tool confere o **magic number**: os primeiros bytes do arquivo têm que ser `0x7F` seguido de `'E'`, `'L'`, `'F'`. Se não bater, ela aborta com uma mensagem clara e retorna código de erro. Isso evita rodar em cima de lixo e interpretar bytes aleatórios como se fossem um ELF.

Na prática, a tool lê um `Elf64_Ehdr` a partir do offset `0` e compara os primeiros `SELFMAG` (4) bytes de `e_ident` com a constante `ELFMAG` (`"\177ELF"`), ambas de `elf.h`:

```c
if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
    /* not an ELF: abort */
}
```

Também tratamos dois erros de borda antes disso: arquivo que não abre e arquivo pequeno demais para conter um header inteiro.

Compilação:

```sh
gcc -Wall -Wextra -o tool/tool tool/tool.c
```

Uso:

```sh
./tool/tool target/target
# ok: 'target/target' is a valid ELF

./tool/tool arquivo-qualquer
# error: 'arquivo-qualquer' not an ELF (invalid magic number)
```

## Status

Em construção. Já existem: o alvo e o passo 1 da tool (validação de ELF). Próximos passos da tool: ler o `Elf64_Ehdr` (OEP e tabela de program headers), localizar o segmento de código (`PT_LOAD` com flag de execução) e o `PT_NOTE` a ser canibalizado. Depois vêm o `stub` e o código compartilhado em `shared/`.
