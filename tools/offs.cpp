#include <GeneralStructures.h>
#include <YRPP.h>
#include <windows.h>
#include <stdio.h>
#include <cstddef>

int main()
{
    printf("sizeof(TechnoClass)=%zu\n", sizeof(TechnoClass));
    printf("TechnoClass::Owner      = 0x%zX\n", offsetof(TechnoClass, Owner));
    printf("TechnoClass::Group      = 0x%zX\n", offsetof(TechnoClass, Group));
    printf("TechnoClass::Focus      = 0x%zX\n", offsetof(TechnoClass, Focus));
    printf("TechnoClass::DisplayProd= 0x%zX\n", offsetof(TechnoClass, DisplayProductionTo));
    return 0;
}
