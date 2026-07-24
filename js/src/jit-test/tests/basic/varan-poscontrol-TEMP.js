// TEMP positive control for the STEP-0 zero-UDF census. DELETE AFTER THE RUN.
// The product tree now has zero UDF emitters, so a "0 UDF" census is only meaningful
// if the pipeline can still SEE a UDF line. varanT2EmitGuards(1) drives the emit-guard
// path -> varanRejectPcField -> emitUdf -> "VARAN-UDF-EMIT code=0x..." on the sim shell.
// Built on scaffolding that emits BY CONSTRUCTION (not a product bug), per the
// vacuous-positive-control lesson.
if (typeof varanT2EmitGuards === "function")
    varanT2EmitGuards(1);
