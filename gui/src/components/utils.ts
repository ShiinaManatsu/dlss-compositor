import React from 'react';

export const handleNumberInputKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
  if (!/^[0-9.-]$/.test(e.key) && !['Backspace', 'Delete', 'ArrowLeft', 'ArrowRight', 'Tab', 'Enter'].includes(e.key)) {
    e.preventDefault();
  }
};

export const flashInput = (e: React.FocusEvent<HTMLInputElement>) => {
  const el = e.target;
  el.classList.add('ring-2', 'ring-yellow-400');
  setTimeout(() => {
    el.classList.remove('ring-2', 'ring-yellow-400');
  }, 500);
};
